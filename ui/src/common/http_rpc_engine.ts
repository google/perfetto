// Copyright (C) 2019 The Android Open Source Project
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

import {fetchWithTimeout} from '../base/http_utils';
import {assertExists} from '../base/logging';
import {StatusResult} from '../common/protos';

import {Engine, LoadingTracker} from './engine';

export const RPC_URL = 'http://127.0.0.1:9001/';
export const WS_URL = 'ws://127.0.0.1:9001/websocket';

const RPC_CONNECT_TIMEOUT_MS = 2000;

export interface HttpRpcState {
  connected: boolean;
  status?: StatusResult;
  failure?: string;
}

export class HttpRpcEngine extends Engine {
  readonly id: string;
  errorHandler: (err: string) => void = () => {};
  private requestQueue = new Array<Uint8Array>();
  private websocket?: WebSocket;
  private connected = false;

  constructor(id: string, loadingTracker?: LoadingTracker) {
    super(loadingTracker);
    this.id = id;
  }

  rpcSendRequestBytes(data: Uint8Array): void {
    if (this.websocket === undefined) {
      this.websocket = new WebSocket(WS_URL);
      this.websocket.onopen = () => this.onWebsocketConnected();
      this.websocket.onmessage = (e) => this.onWebsocketMessage(e);
      this.websocket.onclose = (e) =>
          this.errorHandler(`Websocket closed (${e.code}: ${e.reason})`);
      this.websocket.onerror = (e) =>
          this.errorHandler(`WebSocket error: ${e}`);
    }

    if (this.connected) {
      this.websocket.send(data);
    } else {
      this.requestQueue.push(data);  // onWebsocketConnected() will flush this.
    }
  }

  private onWebsocketConnected() {
    for (;;) {
      const queuedMsg = this.requestQueue.shift();
      if (queuedMsg === undefined) break;
      assertExists(this.websocket).send(queuedMsg);
    }
    this.connected = true;
  }

  private onWebsocketMessage(e: MessageEvent) {
    assertExists(e.data as Blob).arrayBuffer().then((buf) => {
      super.onRpcResponseBytes(new Uint8Array(buf));
    });
  }

  static async checkConnection(): Promise<HttpRpcState> {
    const httpRpcState: HttpRpcState = {connected: false};
    console.info(
        `It's safe to ignore the ERR_CONNECTION_REFUSED on ${RPC_URL} below. ` +
        `That might happen while probing the external native accelerator. The ` +
        `error is non-fatal and unlikely to be the culprit for any UI bug.`);
    try {
      const resp = await fetchWithTimeout(
          RPC_URL + 'status',
          {method: 'post', cache: 'no-cache'},
          RPC_CONNECT_TIMEOUT_MS);
      if (resp.status !== 200) {
        httpRpcState.failure = `${resp.status} - ${resp.statusText}`;
      } else {
        const buf = new Uint8Array(await resp.arrayBuffer());
        httpRpcState.connected = true;
        httpRpcState.status = StatusResult.decode(buf);
      }
    } catch (err) {
      httpRpcState.failure = `${err}`;
    }
    return httpRpcState;
  }
}
