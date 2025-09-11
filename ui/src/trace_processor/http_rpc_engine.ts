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

import protos from '../protos';
import {fetchWithTimeout} from '../base/http_utils';
import {assertExists, reportError} from '../base/logging';
import {EngineBase} from '../trace_processor/engine';

const RPC_CONNECT_TIMEOUT_MS = 2000;

export interface HttpRpcState {
  connected: boolean;
  status?: protos.RpcStatus;
  failure?: string;
}

export class HttpRpcEngine extends EngineBase {
  readonly mode = 'HTTP_RPC';
  readonly id: string;
  private requestQueue = new Array<Uint8Array>();
  private websocket?: WebSocket;
  private connected = false;
  private disposed = false;
  private queue: Blob[] = [];
  private isProcessingQueue = false;
  private trace_processor_uuid = '';
  private isWaitingForUuid = false;

  // Can be changed by frontend/index.ts when passing ?rpc_port=1234 .
  static rpcPort = '9001';

  constructor(id: string, traceProcessorUuid?: string) {
    super();
    this.id = id;
    this.trace_processor_uuid = traceProcessorUuid || '';
  }

  private connect() {
    if (this.websocket !== undefined || this.disposed) return;

    let wsUrl: string;
    if (this.trace_processor_uuid === '') {
      // This is a new session. Ask the server for a new TP instance.
      wsUrl = `ws://${HttpRpcEngine.hostAndPort}/websocket/new`;
      this.isWaitingForUuid = true;
    } else {
      // We have a UUID (e.g. from the page URL), connect to that specific instance.
      wsUrl = `ws://${HttpRpcEngine.hostAndPort}/websocket/${this.trace_processor_uuid}`;
      this.isWaitingForUuid = false;
    }

    this.websocket = new WebSocket(wsUrl);
    this.websocket.onopen = () => this.onWebsocketConnected();
    this.websocket.onmessage = (e) => this.onWebsocketMessage(e);
    this.websocket.onclose = (e) => this.onWebsocketClosed(e);
    this.websocket.onerror = (e) =>
      super.fail(
        `WebSocket error rs=${(e.target as WebSocket)?.readyState} (ERR:ws)`,
      );
  }

  rpcSendRequestBytes(data: Uint8Array): void {
    this.connect(); // Ensures a connection is requested.

    if (this.connected) {
      assertExists(this.websocket).send(data);
    } else {
      this.requestQueue.push(data); // onWebsocketConnected() will flush this.
    }
  }

  onWebsocketConnected() {
    // If we are waiting for the UUID, the connection is not truly "ready"
    // until that UUID has been received. onWebsocketMessage will call this
    // again once the UUID arrives.
    if (this.isWaitingForUuid) return;

    this.connected = true;
    for (;;) {
      const queuedMsg = this.requestQueue.shift();
      if (queuedMsg === undefined) break;
      assertExists(this.websocket).send(queuedMsg);
    }
  }

  private onWebsocketClosed(e: CloseEvent) {
    if (this.disposed) return;
    if (e.code === 1006 && this.connected) {
      // On macbooks the act of closing the lid / suspending often causes socket
      // disconnections. Try to gracefully re-connect.
      console.log('Websocket closed, reconnecting');
      this.websocket = undefined;
      this.connected = false;
      setTimeout(() => {
        if (this.disposed) return;
        this.connect(); // triggers a reconnection after a small delay to prevent race conditions
      }, 200);
    } else {
      super.fail(`Websocket closed (${e.code}: ${e.reason}) (ERR:ws)`);
    }
  }

  private onWebsocketMessage(e: MessageEvent) {
    const blob = assertExists(e.data as Blob);
    if (this.isWaitingForUuid) {
      // The very first message for a 'new' connection contains the UUID.
      blob.text().then((text) => {
        try {
          const data = JSON.parse(text);
          if (data.uuid as string) {
            this.trace_processor_uuid = data.uuid;
            this.isWaitingForUuid = false;
            // Now that we have the UUID and are fully connected,
            // mark the connection as ready and send any queued data.
            this.onWebsocketConnected();
          } else {
            this.fail(`Initial message missing UUID: ${text}`);
          }
        } catch (error) {
          this.fail(`Failed to parse UUID message from server: ${error}`);
        }
      });
      return;
    }

    // Standard RPC message processing.
    this.queue.push(blob);
    this.processQueue();
  }

  private async processQueue() {
    if (this.isProcessingQueue) return;
    this.isProcessingQueue = true;
    while (this.queue.length > 0) {
      try {
        const blob = assertExists(this.queue.shift());
        const buf = await blob.arrayBuffer();
        super.onRpcResponseBytes(new Uint8Array(buf));
      } catch (e) {
        reportError(e);
      }
    }
    this.isProcessingQueue = false;
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
        httpRpcState.status = protos.RpcStatus.decode(buf);
        httpRpcState.connected = true;
      }
    } catch (err) {
      httpRpcState.failure = `${err}`;
    }
    return httpRpcState;
  }

  static get hostAndPort() {
    return `${window.location.hostname}:${HttpRpcEngine.rpcPort}`;
  }

  [Symbol.dispose]() {
    this.disposed = true;
    this.connected = false;
    const websocket = this.websocket;
    this.websocket = undefined;
    websocket?.close();
  }
}
