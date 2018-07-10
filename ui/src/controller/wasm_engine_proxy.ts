// Copyright (C) 2018 The Android Open Source Project
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

import * as protobufjs from 'protobufjs/light';

import {TraceProcessor} from '../common/protos';
import {WasmBridgeRequest, WasmBridgeResponse} from '../engine/wasm_bridge';

import {Engine} from './engine';

let gWarmWasmWorker: null|Worker = null;

function createNewWasmEngineWorker(): Worker {
  return new Worker('engine_bundle.js');
}

function createWasmEngineWorker(): Worker {
  if (gWarmWasmWorker === null) {
    return createNewWasmEngineWorker();
  }
  const worker = gWarmWasmWorker;
  gWarmWasmWorker = createNewWasmEngineWorker();
  return worker;
}

/**
 * It's quite slow to compile WASM and (in Chrome) this happens every time
 * a worker thread attempts to load a WASM module since there is no way to
 * cache the compiled code currently. To mitigate this we can always keep a
 * WASM backend 'ready to go' just waiting to be provided with a trace file.
 * warmupWasmEngineWorker (together with getWasmEngineWorker)
 * implement this behaviour.
 */
export function warmupWasmEngineWorker(): void {
  if (gWarmWasmWorker !== null) {
    return;
  }
  gWarmWasmWorker = createNewWasmEngineWorker();
}

/**
 * This implementation of Engine uses a WASM backend hosted in a seperate
 * worker thread.
 */
export class WasmEngineProxy extends Engine {
  private worker: Worker;
  private readonly traceProcessor_: TraceProcessor;
  private pendingCallbacks: Map<number, protobufjs.RPCImplCallback>;
  private nextRequestId: number;

  static create(blob: Blob): Engine {
    const worker = createWasmEngineWorker();
    // tslint:disable-next-line deprecation
    worker.postMessage({
      blob,
    });
    return new WasmEngineProxy(worker);
  }

  constructor(worker: Worker) {
    super();
    this.nextRequestId = 0;
    this.pendingCallbacks = new Map();
    this.worker = worker;
    this.worker.onerror = this.onError.bind(this);
    this.worker.onmessage = this.onMessage.bind(this);
    this.traceProcessor_ =
        TraceProcessor.create(this.rpcImpl.bind(this, 'trace_processor'));
  }

  get traceProcessor(): TraceProcessor {
    return this.traceProcessor_;
  }

  onError(e: ErrorEvent) {
    console.error(e);
  }

  onMessage(m: MessageEvent) {
    const response = m.data as WasmBridgeResponse;
    const callback = this.pendingCallbacks.get(response.id);
    if (callback === undefined) {
      throw new Error(`No such request: ${response.id}`);
    }
    this.pendingCallbacks.delete(response.id);
    callback(null, response.data);
  }

  rpcImpl(
      serviceName: string, method: Function, requestData: Uint8Array,
      callback: protobufjs.RPCImplCallback): void {
    const methodName = method.name;
    const id = this.nextRequestId++;
    this.pendingCallbacks.set(id, callback);
    const request: WasmBridgeRequest = {
      id,
      serviceName,
      methodName,
      data: requestData,
    };
    // tslint:disable-next-line deprecation
    this.worker.postMessage(request);
  }
}
