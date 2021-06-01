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

import {assertTrue} from '../base/logging';
import {Engine, LoadingTracker} from './engine';

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
 * This implementation of Engine uses a WASM backend hosted in a separate
 * worker thread.
 */
export class WasmEngineProxy extends Engine {
  readonly id: string;
  private readonly worker: Worker;

  constructor(id: string, worker: Worker, loadingTracker?: LoadingTracker) {
    super(loadingTracker);
    this.id = id;
    this.worker = worker;
    this.worker.onmessage = this.onMessage.bind(this);
  }

  onMessage(m: MessageEvent) {
    assertTrue(m.data instanceof Uint8Array);
    super.onRpcResponseBytes(m.data as Uint8Array);
  }

  rpcSendRequestBytes(data: Uint8Array): void {
    // We deliberately don't use a transfer list because protobufjs reuses the
    // same buffer when encoding messages (which is good, because creating a new
    // TypedArray for each decode operation would be too expensive).
    this.worker.postMessage(data);
  }
}
