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
import {assertTrue} from '../base/logging';
import {StatusResult} from '../common/protos';

import {Engine, LoadingTracker} from './engine';

export const RPC_URL = 'http://127.0.0.1:9001/';
const RPC_CONNECT_TIMEOUT_MS = 2000;

export interface HttpRpcState {
  connected: boolean;
  status?: StatusResult;
  failure?: string;
}

export class HttpRpcEngine extends Engine {
  readonly id: string;
  private requestQueue = new Array<Uint8Array>();
  private requestPending = false;
  errorHandler: (err: string) => void = () => {};

  constructor(id: string, loadingTracker?: LoadingTracker) {
    super(loadingTracker);
    this.id = id;
  }

  rpcSendRequestBytes(data: Uint8Array): void {
    if (!this.requestPending && this.requestQueue.length === 0) {
      this.beginFetch(data);
    } else {
      this.requestQueue.push(data);
    }
  }

  private beginFetch(data: Uint8Array) {
    assertTrue(!this.requestPending);
    this.requestPending = true;
    // Deliberately not using fetchWithTimeout() here. These queries can be
    // arbitrarily long.
    // Deliberately not setting cache: no-cache. Doing so invalidates also the
    // CORS pre-flight responses, causing one OPTIONS request for each POST.
    // no-cache is also useless because trace-processor's replies are already
    // marked as no-cache and browsers generally already assume that POST
    // requests are not idempotent.
    fetch(RPC_URL + 'rpc', {
      method: 'post',
      headers: {'Content-Type': 'application/x-protobuf'},
      body: data,
    })
        .then(resp => this.endFetch(resp))
        .catch(err => this.errorHandler(err));
  }

  private endFetch(resp: Response) {
    assertTrue(this.requestPending);
    if (resp.status !== 200) {
      throw new Error(`HTTP ${resp.status} - ${resp.statusText}`);
    }
    resp.arrayBuffer().then(arrBuf => {
      // Note: another request can sneak in via enqueueRequest() between the
      // arrayBuffer() call and this continuation. At this point
      // this.pendingRequest might be set again.
      // If not (the most common case) submit the next queued request, if any.
      this.requestPending = false;
      if (this.requestQueue.length > 0) {
        this.beginFetch(this.requestQueue.shift()!);
      }
      super.onRpcResponseBytes(new Uint8Array(arrBuf));
    });
  }

  static async checkConnection(): Promise<HttpRpcState> {
    const httpRpcState: HttpRpcState = {connected: false};
    console.info(
        `It's safe to ignore the ERR_CONNECTION_REFUSED on ${RPC_URL} below. ` +
        `That might happen while probing the exernal native accelerator. The ` +
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
