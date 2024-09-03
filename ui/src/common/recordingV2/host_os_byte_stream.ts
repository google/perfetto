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

import {defer} from '../../base/deferred';
import {
  ByteStream,
  OnStreamCloseCallback,
  OnStreamDataCallback,
} from './recording_interfaces_v2';

// A HostOsByteStream instantiates a websocket connection to the host OS.
// It exposes an API to write commands to this websocket and read its output.
export class HostOsByteStream implements ByteStream {
  // handshakeSignal will be resolved with the stream when the websocket
  // connection becomes open.
  private handshakeSignal = defer<HostOsByteStream>();
  private _isConnected: boolean = false;
  private websocket: WebSocket;
  private onStreamDataCallbacks: OnStreamDataCallback[] = [];
  private onStreamCloseCallbacks: OnStreamCloseCallback[] = [];

  private constructor(websocketUrl: string) {
    this.websocket = new WebSocket(websocketUrl);
    this.websocket.onmessage = this.onMessage.bind(this);
    this.websocket.onopen = this.onOpen.bind(this);
  }

  addOnStreamDataCallback(onStreamData: OnStreamDataCallback): void {
    this.onStreamDataCallbacks.push(onStreamData);
  }

  addOnStreamCloseCallback(onStreamClose: OnStreamCloseCallback): void {
    this.onStreamCloseCallbacks.push(onStreamClose);
  }

  close(): void {
    this.websocket.close();
    for (const onStreamClose of this.onStreamCloseCallbacks) {
      onStreamClose();
    }
    this.onStreamDataCallbacks = [];
    this.onStreamCloseCallbacks = [];
  }

  async closeAndWaitForTeardown(): Promise<void> {
    this.close();
  }

  isConnected(): boolean {
    return this._isConnected;
  }

  write(msg: string | Uint8Array): void {
    this.websocket.send(msg);
  }

  private async onMessage(evt: MessageEvent) {
    for (const onStreamData of this.onStreamDataCallbacks) {
      const arrayBufferResponse = await evt.data.arrayBuffer();
      onStreamData(new Uint8Array(arrayBufferResponse));
    }
  }

  private onOpen() {
    this._isConnected = true;
    this.handshakeSignal.resolve(this);
  }

  static create(websocketUrl: string): Promise<HostOsByteStream> {
    return new HostOsByteStream(websocketUrl).handshakeSignal;
  }
}
