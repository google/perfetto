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

import {assertExists, assertTrue} from '../base/logging';

import {Engine, LoadingTracker} from './engine';

let bundlePath: string;
let idleWasmWorker: Worker;
let activeWasmWorker: Worker;

export function initWasm(root: string) {
  bundlePath = root + 'engine_bundle.js';
  idleWasmWorker = new Worker(bundlePath);
}

// This method is called trace_controller whenever a new trace is loaded.
export function resetEngineWorker(): MessagePort {
  const channel = new MessageChannel();
  const port = channel.port1;

  // We keep always an idle worker around, the first one is created by the
  // main() below, so we can hide the latency of the Wasm initialization.
  if (activeWasmWorker !== undefined) {
    activeWasmWorker.terminate();
  }

  // Swap the active worker with the idle one and create a new idle worker
  // for the next trace.
  activeWasmWorker = assertExists(idleWasmWorker);
  activeWasmWorker.postMessage(port, [port]);
  idleWasmWorker = new Worker(bundlePath);
  return channel.port2;
}

/**
 * This implementation of Engine uses a WASM backend hosted in a separate
 * worker thread.
 */
export class WasmEngineProxy extends Engine {
  readonly id: string;
  private port: MessagePort;

  constructor(id: string, port: MessagePort, loadingTracker?: LoadingTracker) {
    super(loadingTracker);
    this.id = id;
    this.port = port;
    this.port.onmessage = this.onMessage.bind(this);
  }

  onMessage(m: MessageEvent) {
    assertTrue(m.data instanceof Uint8Array);
    super.onRpcResponseBytes(m.data as Uint8Array);
  }

  rpcSendRequestBytes(data: Uint8Array): void {
    // We deliberately don't use a transfer list because protobufjs reuses the
    // same buffer when encoding messages (which is good, because creating a new
    // TypedArray for each decode operation would be too expensive).
    this.port.postMessage(data);
  }
}
