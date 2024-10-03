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
import {EngineBase} from '../trace_processor/engine';

let bundlePath: string;
let idleWasmWorker: Worker;

export function initWasm(root: string) {
  bundlePath = root + 'engine_bundle.js';
  idleWasmWorker = new Worker(bundlePath);
}

/**
 * This implementation of Engine uses a WASM backend hosted in a separate
 * worker thread. The entrypoint of the worker thread is engine/index.ts.
 */
export class WasmEngineProxy extends EngineBase implements Disposable {
  readonly mode = 'WASM';
  readonly id: string;
  private port: MessagePort;
  private worker: Worker;

  constructor(id: string) {
    super();
    this.id = id;

    const channel = new MessageChannel();
    const port1 = channel.port1;
    this.port = channel.port2;

    // We keep an idle instance around to hide the latency of initializing the
    // instance. Creating the worker (new Worker()) is ~instantaneous, but then
    // the initialization in the worker thread (i.e. the call to
    // `new WasmBridge()` that engine/index.ts makes) takes several seconds.
    // Here we hide that initialization latency by always keeping an idle worker
    // around. The latency is hidden by the fact that the user usually takes few
    // seconds until they click on "open trace file" and pick a file.
    this.worker = assertExists(idleWasmWorker);
    idleWasmWorker = new Worker(bundlePath);
    this.worker.postMessage(port1, [port1]);
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

  [Symbol.dispose]() {
    this.worker.terminate();
  }
}
