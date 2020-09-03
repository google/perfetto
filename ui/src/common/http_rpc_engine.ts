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

import {defer, Deferred} from '../base/deferred';
import {fetchWithTimeout} from '../base/http_utils';
import {assertExists, assertTrue} from '../base/logging';
import {StatusResult} from '../common/protos';

import {Engine, LoadingTracker} from './engine';

export const RPC_URL = 'http://127.0.0.1:9001/';
const RPC_CONNECT_TIMEOUT_MS = 2000;

export interface HttpRpcState {
  connected: boolean;
  loadedTraceName?: string;
  failure?: string;
}

interface QueuedRequest {
  methodName: string;
  reqData?: Uint8Array;
  resp: Deferred<Uint8Array>;
  id: number;
}

export class HttpRpcEngine extends Engine {
  readonly id: string;
  private nextReqId = 0;
  private requestQueue = new Array<QueuedRequest>();
  private pendingRequest?: QueuedRequest = undefined;
  errorHandler: (err: string) => void = () => {};

  constructor(id: string, loadingTracker?: LoadingTracker) {
    super(loadingTracker);
    this.id = id;
  }

  async parse(data: Uint8Array): Promise<void> {
    await this.enqueueRequest('parse', data);
  }

  async notifyEof(): Promise<void> {
    await this.enqueueRequest('notify_eof');
  }

  async restoreInitialTables(): Promise<void> {
    await this.enqueueRequest('restore_initial_tables');
  }

  rawQuery(rawQueryArgs: Uint8Array): Promise<Uint8Array> {
    return this.enqueueRequest('raw_query', rawQueryArgs);
  }

  rawComputeMetric(rawComputeMetricArgs: Uint8Array): Promise<Uint8Array> {
    return this.enqueueRequest('compute_metric', rawComputeMetricArgs);
  }

  async enableMetatrace(): Promise<void> {
    await this.enqueueRequest('enable_metatrace');
  }

  disableAndReadMetatrace(): Promise<Uint8Array> {
    return this.enqueueRequest('disable_and_read_metatrace');
  }

  enqueueRequest(methodName: string, data?: Uint8Array): Promise<Uint8Array> {
    const resp = defer<Uint8Array>();
    const req:
        QueuedRequest = {methodName, reqData: data, resp, id: this.nextReqId++};
    if (this.pendingRequest === undefined) {
      this.beginFetch(req);
    } else {
      this.requestQueue.push(req);
    }
    return resp;
  }

  private beginFetch(req: QueuedRequest) {
    assertTrue(this.pendingRequest === undefined);
    this.pendingRequest = req;
    const methodName = req.methodName.toLowerCase();
    // Deliberately not using fetchWithTimeout() here. These queries can be
    // arbitrarily long.
    // Deliberately not setting cache: no-cache. Doing so invalidates also the
    // CORS pre-flight responses, causing one OPTIONS request for each POST.
    // no-cache is also useless because trace-processor's replies are already
    // marked as no-cache and browsers generally already assume that POST
    // requests are not idempotent.
    fetch(RPC_URL + methodName, {
      method: 'post',
      headers: {
        'Content-Type': 'application/x-protobuf',
        'X-Seq-Id': `${req.id}`,  // Used only for debugging.
      },
      body: req.reqData || new Uint8Array(),
    })
        .then(resp => this.endFetch(resp, req.id))
        .catch(err => this.errorHandler(err));
  }

  private endFetch(resp: Response, expectedReqId: number) {
    const req = assertExists(this.pendingRequest);
    this.pendingRequest = undefined;
    assertTrue(expectedReqId === req.id);
    if (resp.status !== 200) {
      req.resp.reject(`HTTP ${resp.status} - ${resp.statusText}`);
      return;
    }
    resp.arrayBuffer().then(arrBuf => {
      // Note: another request can sneak in via enqueueRequest() between the
      // arrayBuffer() call and this continuation. At this point
      // this.pendingRequest might be set again.
      // If not (the most common case) submit the next queued request, if any.
      this.maybeSubmitNextQueuedRequest();
      req.resp.resolve(new Uint8Array(arrBuf));
    });
  }

  private maybeSubmitNextQueuedRequest() {
    if (this.pendingRequest === undefined && this.requestQueue.length > 0) {
      this.beginFetch(this.requestQueue.shift()!);
    }
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
        const status = StatusResult.decode(buf);
        httpRpcState.connected = true;
        if (status.loadedTraceName) {
          httpRpcState.loadedTraceName = status.loadedTraceName;
        }
      }
    } catch (err) {
      httpRpcState.failure = `${err}`;
    }
    return httpRpcState;
  }
}
