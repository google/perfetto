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

import {defer} from '../base/deferred';
import {TraceProcessor} from '../common/protos';
import {WasmBridgeRequest, WasmBridgeResponse} from '../engine/wasm_bridge';

import {Engine} from './engine';

const activeWorkers = new Map<string, Worker>();
let warmWorker: null|Worker = null;

function createWorker(): Worker {
  return new Worker('engine_bundle.js');
}

// Take the warm engine and start creating a new WASM engine in the background
// for the next call.
export function createWasmEngine(id: string): Worker {
  if (warmWorker === null) {
    throw new Error('warmupWasmEngine() not called');
  }
  if (activeWorkers.has(id)) {
    throw new Error(`Duplicate worker ID ${id}`);
  }
  const activeWorker = warmWorker;
  warmWorker = createWorker();
  activeWorkers.set(id, activeWorker);
  return activeWorker;
}

export function destroyWasmEngine(id: string) {
  if (!activeWorkers.has(id)) {
    throw new Error(`Cannot find worker ID ${id}`);
  }
  activeWorkers.get(id)!.terminate();
  activeWorkers.delete(id);
}

/**
 * It's quite slow to compile WASM and (in Chrome) this happens every time
 * a worker thread attempts to load a WASM module since there is no way to
 * cache the compiled code currently. To mitigate this we can always keep a
 * WASM backend 'ready to go' just waiting to be provided with a trace file.
 * warmupWasmEngineWorker (together with getWasmEngineWorker)
 * implement this behaviour.
 */
export function warmupWasmEngine(): void {
  if (warmWorker !== null) {
    throw new Error('warmupWasmEngine() already called');
  }
  warmWorker = createWorker();
}

/**
 * This implementation of Engine uses a WASM backend hosted in a seperate
 * worker thread.
 */
export class WasmEngineProxy extends Engine {
  private readonly worker: Worker;
  private readonly traceProcessor_: TraceProcessor;
  private pendingCallbacks: Map<number, protobufjs.RPCImplCallback>;
  private nextRequestId: number;
  readonly id: string;

  constructor(args: {id: string, worker: Worker}) {
    super();
    this.nextRequestId = 0;
    this.pendingCallbacks = new Map();
    this.id = args.id;
    this.worker = args.worker;
    this.worker.onmessage = this.onMessage.bind(this);
    this.traceProcessor_ =
        TraceProcessor.create(this.rpcImpl.bind(this, 'trace_processor'));
  }

  get rpc(): TraceProcessor {
    return this.traceProcessor_;
  }

  parse(data: Uint8Array): Promise<void> {
    const id = this.nextRequestId++;
    const request: WasmBridgeRequest =
        {id, serviceName: 'trace_processor', methodName: 'parse', data};
    const promise = defer<void>();
    this.pendingCallbacks.set(id, () => promise.resolve());
    this.worker.postMessage(request);
    return promise;
  }

  notifyEof(): Promise<void> {
    const id = this.nextRequestId++;
    const data = Uint8Array.from([]);
    const request: WasmBridgeRequest =
        {id, serviceName: 'trace_processor', methodName: 'notifyEof', data};
    const promise = defer<void>();
    this.pendingCallbacks.set(id, () => promise.resolve());
    this.worker.postMessage(request);
    return promise;
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
    this.worker.postMessage(request);
  }
}
