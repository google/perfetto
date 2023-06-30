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

const RPC_CONNECT_TIMEOUT_MS = 2000;

export function getRPC_URL(port: number) {
  return `http://127.0.0.1:${port}/`;
}

export function getWS_URL(port: number) {
  return `ws://127.0.0.1:${port}/websocket`;
}

export interface HttpRpcState {
  connected: boolean;
  status?: StatusResult;
  failure?: string;
}

/** A call-back to customize a newly created HTTP+RPC engine. */
export type HttpRcpEngineCustomizer = (engine: HttpRpcEngine) => unknown;

export class HttpRpcEngine extends Engine {
  readonly id: string;
  readonly port: number;
  errorHandler: (err: string) => void = () => {};
  closeHandler: (code: number, reason: string) => void = (code, reason) => this.errorHandler(`Websocket closed (${code}: ${reason})`);
  private requestQueue = new Array<Uint8Array>();
  private websocket?: WebSocket;
  private connected = false;

  constructor(id: string, loadingTracker?: LoadingTracker, port = 9001) {
    super(loadingTracker);
    this.id = id;
    this.port = port;
  }

  rpcSendRequestBytes(data: Uint8Array): void {
    if (this.websocket === undefined) {
      this.websocket = new WebSocket(getWS_URL(this.port));
      this.websocket.onopen = () => this.onWebsocketConnected();
      this.websocket.onmessage = (e) => this.onWebsocketMessage(e);
      this.websocket.onclose = (e) =>
          this.closeHandler(e.code, e.reason);
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

  static async checkConnection(port: number): Promise<HttpRpcState> {
    const httpRpcState: HttpRpcState = {connected: false};
    console.info(
        `It's safe to ignore the ERR_CONNECTION_REFUSED on ${getRPC_URL(port)} below. ` +
        `That might happen while probing the external native accelerator. The ` +
        `error is non-fatal and unlikely to be the culprit for any UI bug.`);
    try {
      const resp = await fetchWithTimeout(
          getRPC_URL(port) + 'status',
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
