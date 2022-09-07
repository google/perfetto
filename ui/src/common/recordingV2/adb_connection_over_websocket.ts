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

import {defer, Deferred} from '../../base/deferred';
import {ArrayBufferBuilder} from '../array_buffer_builder';

import {buildAbdWebsocketCommand} from './adb_over_websocket_utils';
import {ALLOW_USB_DEBUGGING} from './adb_targets_utils';
import {RecordingError} from './recording_error_handling';
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

  push(_binary: ArrayBuffer, _path: string): Promise<void> {
    // TODO(octaviant) implement
    throw new Error('Not implemented yet');
  }

  // Starts a shell command, then gathers all its output and returns it as
  // a string.
  async shellAndGetOutput(cmd: string): Promise<string> {
    const adbStream = await this.shell(cmd);
    const commandOutput = new ArrayBufferBuilder();
    const onStreamingEnded = defer<string>();

    adbStream.addOnStreamData((data: Uint8Array) => {
      commandOutput.append(data);
    });
    adbStream.addOnStreamClose(() => {
      onStreamingEnded.resolve(
          textDecoder.decode(commandOutput.toArrayBuffer()));
    });
    return onStreamingEnded;
  }

  shell(cmd: string): Promise<AdbOverWebsocketStream> {
    return this.openStream('shell:' + cmd);
  }

  connectSocket(path: string): Promise<AdbOverWebsocketStream> {
    return this.openStream(path);
  }

  private async openStream(destination: string):
      Promise<AdbOverWebsocketStream> {
    return AdbOverWebsocketStream.create(
        this.websocketUrl,
        destination,
        this.deviceSerialNumber,
        this.closeStream.bind(this));
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

  // There will be no contention for the websocket connection, because it will
  // communicate with the 'adb server' running on the computer which opened
  // Perfetto.
  canConnectWithoutContention(): Promise<boolean> {
    return Promise.resolve(true);
  }
}

// An AdbOverWebsocketStream instantiates a websocket connection to the device.
// It exposes an API to write commands to this websocket and read its output.
export class AdbOverWebsocketStream implements ByteStream {
  private websocket: WebSocket;
  // commandSentSignal gets resolved if we successfully connect to the device
  // and send the command this socket wraps. commandSentSignal gets rejected if
  // we fail to connect to the device.
  private commandSentSignal = defer<AdbOverWebsocketStream>();
  // We store a promise for each messge while the message is processed.
  // This way, if the websocket server closes the connection, we first process
  // all previously received messages and only afterwards disconnect.
  // An application is when the stream wraps a shell command. The websocket
  // server will reply and then immediately disconnect.
  private messageProcessedSignals: Set<Deferred<void>> = new Set();

  private _isConnected = false;
  private onStreamDataCallbacks: OnStreamDataCallback[] = [];
  private onStreamCloseCallbacks: OnStreamCloseCallback[] = [];

  private constructor(
      websocketUrl: string, destination: string, deviceSerialNumber: string,
      private removeFromConnection: (stream: AdbOverWebsocketStream) => void) {
    this.websocket = new WebSocket(websocketUrl);

    this.websocket.onopen = this.onOpen.bind(this, deviceSerialNumber);
    this.websocket.onmessage = this.onMessage.bind(this, destination);
    // The websocket may be closed by the websocket server. This happens
    // for instance when we get the full result of a shell command.
    this.websocket.onclose = this.onClose.bind(this);
  }

  addOnStreamData(onStreamData: OnStreamDataCallback) {
    this.onStreamDataCallbacks.push(onStreamData);
  }

  addOnStreamClose(onStreamClose: OnStreamCloseCallback) {
    this.onStreamCloseCallbacks.push(onStreamClose);
  }

  // Used by the connection object to signal newly received data, not exposed
  // in the interface.
  signalStreamData(data: Uint8Array): void {
    for (const onStreamData of this.onStreamDataCallbacks) {
      onStreamData(data);
    }
  }

  // Used by the connection object to signal the stream is closed, not exposed
  // in the interface.
  signalStreamClosed(): void {
    for (const onStreamClose of this.onStreamCloseCallbacks) {
      onStreamClose();
    }
    this.onStreamDataCallbacks = [];
    this.onStreamCloseCallbacks = [];
  }

  // We close the websocket and notify the AdbConnection to remove this stream.
  close(): void {
    // If the websocket connection is still open (ie. the close did not
    // originate from the server), we close the websocket connection.
    if (this.websocket.readyState === this.websocket.OPEN) {
      this.websocket.close();
      // We remove the 'onclose' callback so the 'close' method doesn't get
      // executed twice.
      this.websocket.onclose = null;
    }
    this._isConnected = false;
    this.removeFromConnection(this);
    this.signalStreamClosed();
  }

  write(msg: string|Uint8Array): void {
    this.websocket.send(msg);
  }

  isConnected(): boolean {
    return this._isConnected;
  }

  private async onOpen(deviceSerialNumber: string): Promise<void> {
    this.websocket.send(
        buildAbdWebsocketCommand(`host:transport:${deviceSerialNumber}`));
  }

  private async onMessage(destination: string, evt: MessageEvent):
      Promise<void> {
    const messageProcessed = defer<void>();
    this.messageProcessedSignals.add(messageProcessed);
    try {
      if (!this._isConnected) {
        const txt = await evt.data.text();
        const prefix = txt.substr(0, 4);
        if (prefix === 'OKAY') {
          this._isConnected = true;
          this.websocket.send(buildAbdWebsocketCommand(destination));
          this.commandSentSignal.resolve(this);
        } else if (prefix === 'FAIL' && txt.includes('device unauthorized')) {
          this.commandSentSignal.reject(
              new RecordingError(ALLOW_USB_DEBUGGING));
          this.close();
        } else {
          this.commandSentSignal.reject(
              new RecordingError(WEBSOCKET_UNABLE_TO_CONNECT));
          this.close();
        }
      } else {
        // Upon a successful connection we first receive an 'OKAY' message.
        // After that, we receive messages with traced binary payloads.
        const arrayBufferResponse = await evt.data.arrayBuffer();
        if (textDecoder.decode(arrayBufferResponse) !== 'OKAY') {
          this.signalStreamData(new Uint8Array(arrayBufferResponse));
        }
      }
      messageProcessed.resolve();
    } finally {
      this.messageProcessedSignals.delete(messageProcessed);
    }
  }

  private async onClose(): Promise<void> {
    // Wait for all messages to be processed before closing the connection.
    await Promise.allSettled(this.messageProcessedSignals);
    this.close();
  }

  static create(
      websocketUrl: string, destination: string, deviceSerialNumber: string,
      removeFromConnection: (stream: AdbOverWebsocketStream) => void):
      Promise<AdbOverWebsocketStream> {
    return (new AdbOverWebsocketStream(
                websocketUrl,
                destination,
                deviceSerialNumber,
                removeFromConnection))
        .commandSentSignal;
  }
}
