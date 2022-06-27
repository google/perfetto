// Copyright (C) 2022 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {_TextDecoder} from 'custom_utils';

import {defer} from '../../base/deferred';

import {buildAbdWebsocketCommand} from './adb_over_websocket_utils';
import {ALLOW_USB_DEBUGGING} from './recording_error_handling';
import {
  AdbConnection,
  ByteStream,
  OnDisconnectCallback,
  OnStreamCloseCallback,
  OnStreamDataCallback,
} from './recording_interfaces_v2';

const textDecoder = new _TextDecoder();

export const WEBSOCKET_UNABLE_TO_CONNECT =
    'Unable to connect to device via websocket.';

export class AdbConnectionOverWebsocket implements AdbConnection {
  private streams = new Set<AdbOverWebsocketStream>();

  onDisconnect: OnDisconnectCallback = (_) => {};

  constructor(
      private deviceSerialNumber: string, private websocketUrl: string) {}

  async connectSocket(path: string): Promise<ByteStream> {
    const webSocket = new WebSocket(this.websocketUrl);
    const byteStream =
        new AdbOverWebsocketStream(webSocket, this.closeStream.bind(this));
    const byteStreamConnected = defer<AdbOverWebsocketStream>();

    webSocket.onopen = () => webSocket.send(
        buildAbdWebsocketCommand(`host:transport:${this.deviceSerialNumber}`));

    webSocket.onmessage = async (evt) => {
      if (!byteStream.isOpen()) {
        const txt = await evt.data.text();
        const prefix = txt.substr(0, 4);
        if (prefix === 'OKAY') {
          byteStream.setStreamOpen();
          webSocket.send(buildAbdWebsocketCommand(`localfilesystem:${path}`));
          byteStreamConnected.resolve(byteStream);
        } else if (prefix === 'FAIL' && txt.includes('device unauthorized')) {
          byteStreamConnected.reject(ALLOW_USB_DEBUGGING);
        } else {
          byteStreamConnected.reject(WEBSOCKET_UNABLE_TO_CONNECT);
        }
        return;
      }

      // Upon a successful connection we first receive an 'OKAY' message.
      // After that, we receive messages with traced binary payloads.
      const arrayBufferResponse = await evt.data.arrayBuffer();
      if (textDecoder.decode(arrayBufferResponse) !== 'OKAY') {
        byteStream.onStreamData(new Uint8Array(arrayBufferResponse));
      }
    };

    return byteStreamConnected;
  }

  disconnect(): void {
    for (const stream of this.streams) {
      stream.close();
    }
    this.onDisconnect();
  }

  closeStream(stream: AdbOverWebsocketStream): void {
    if (this.streams.has(stream)) {
      this.streams.delete(stream);
    }
  }
}

// An AdbOverWebsocketStream is instantiated after the creation of a socket to
// the device. Thanks to this, we can send commands and receive their output.
// Messages are received in the main adb class, and are forwarded to an instance
// of this class based on a stream id match.
export class AdbOverWebsocketStream implements ByteStream {
  private _isOpen = false;
  onStreamData: OnStreamDataCallback = (_) => {};
  onStreamClose: OnStreamCloseCallback = () => {};

  constructor(
      private websocket: WebSocket,
      private removeFromConnection: (stream: AdbOverWebsocketStream) => void) {}

  // We close the websocket and notify the AdbConnection to remove this stream.
  close(): void {
    this.websocket.close();
    this._isOpen = false;
    this.removeFromConnection(this);
    this.onStreamClose();
  }

  write(msg: string|Uint8Array): void {
    this.websocket.send(msg);
  }

  isOpen(): boolean {
    return this._isOpen;
  }

  setStreamOpen(): void {
    this._isOpen = true;
  }
}
