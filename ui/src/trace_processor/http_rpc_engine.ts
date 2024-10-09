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
import {StatusResult} from '../protos';
import {EngineBase} from '../trace_processor/engine';

const RPC_CONNECT_TIMEOUT_MS = 2000;

export interface HttpRpcState {
  connected: boolean;
  status?: StatusResult;
  failure?: string;
}

export class HttpRpcEngine extends EngineBase {
  readonly mode = 'HTTP_RPC';
  readonly id: string;
  private requestQueue = new Array<Uint8Array>();
  private websocket?: WebSocket;
  private connected = false;
  private disposed = false;

  // Can be changed by frontend/index.ts when passing ?rpc_port=1234 .
  static rpcPort = '9001';

  constructor(id: string) {
    super();
    this.id = id;
  }

  rpcSendRequestBytes(data: Uint8Array): void {
    if (this.websocket === undefined) {
      if (this.disposed) return;
      const wsUrl = `ws://${HttpRpcEngine.hostAndPort}/websocket`;
      this.websocket = new WebSocket(wsUrl);
      this.websocket.onopen = () => this.onWebsocketConnected();
      this.websocket.onmessage = (e) => this.onWebsocketMessage(e);
      this.websocket.onclose = (e) => this.onWebsocketClosed(e);
      this.websocket.onerror = (e) =>
        super.fail(
          `WebSocket error rs=${(e.target as WebSocket)?.readyState} (ERR:ws)`,
        );
    }

    if (this.connected) {
      this.websocket.send(data);
    } else {
      this.requestQueue.push(data); // onWebsocketConnected() will flush this.
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

  private onWebsocketClosed(e: CloseEvent) {
    if (this.disposed) return;
    if (e.code === 1006 && this.connected) {
      // On macbooks the act of closing the lid / suspending often causes socket
      // disconnections. Try to gracefully re-connect.
      console.log('Websocket closed, reconnecting');
      this.websocket = undefined;
      this.connected = false;
      this.rpcSendRequestBytes(new Uint8Array()); // Triggers a reconnection.
    } else {
      super.fail(`Websocket closed (${e.code}: ${e.reason}) (ERR:ws)`);
    }
  }

  private onWebsocketMessage(e: MessageEvent) {
    assertExists(e.data as Blob)
      .arrayBuffer()
      .then((buf) => {
        super.onRpcResponseBytes(new Uint8Array(buf));
      });
  }

  static async checkConnection(): Promise<HttpRpcState> {
    const RPC_URL = `http://${HttpRpcEngine.hostAndPort}/`;
    const httpRpcState: HttpRpcState = {connected: false};
    console.info(
      `It's safe to ignore the ERR_CONNECTION_REFUSED on ${RPC_URL} below. ` +
        `That might happen while probing the external native accelerator. The ` +
        `error is non-fatal and unlikely to be the culprit for any UI bug.`,
    );
    try {
      const resp = await fetchWithTimeout(
        RPC_URL + 'status',
        {method: 'post', cache: 'no-cache'},
        RPC_CONNECT_TIMEOUT_MS,
      );
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

  static get hostAndPort() {
    return `127.0.0.1:${HttpRpcEngine.rpcPort}`;
  }

  [Symbol.dispose]() {
    this.disposed = true;
    const websocket = this.websocket;
    this.websocket = undefined;
    websocket?.close();
  }
}
