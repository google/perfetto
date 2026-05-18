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

import {assetSrc} from '../base/assets';
import {assertTrue} from '../base/assert';
import {EngineBase} from '../trace_processor/engine';
import {
  TRACE_PROCESSOR_32_WASM_URL,
  TRACE_PROCESSOR_64_WASM_URL,
} from './wasm_modules';

let idleWasmWorker: Worker | undefined = undefined;

// Detected once for the whole page. Used to pick the .wasm URL and forwarded
// to every worker via the bootstrap message.
const USE_MEMORY64 = detectMemory64Support();

function detectMemory64Support(): boolean {
  // Compiled bytes for `(module (memory i64 0))`.
  const program = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x05, 0x03, 0x01, 0x04,
    0x00, 0x00, 0x08, 0x04, 0x6e, 0x61, 0x6d, 0x65, 0x02, 0x01, 0x00,
  ]);
  try {
    new WebAssembly.Module(program);
    return true;
  } catch (e) {
    return false;
  }
}

// Compiled once on the main thread and shared with every worker we spawn so
// V8 reuses the same tiered-up wasm code across workers.
const precompiledWasmModule: Promise<WebAssembly.Module> =
  precompileTraceProcessorWasm();

function precompileTraceProcessorWasm(): Promise<WebAssembly.Module> {
  const wasmUrl = USE_MEMORY64
    ? assetSrc(TRACE_PROCESSOR_64_WASM_URL)
    : assetSrc(TRACE_PROCESSOR_32_WASM_URL);
  return WebAssembly.compileStreaming(fetch(wasmUrl));
}

export function warmupWasmWorker() {
  if (idleWasmWorker === undefined) {
    idleWasmWorker = new Worker(assetSrc('engine_bundle.js'));
  }
  return idleWasmWorker;
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
    this.worker = warmupWasmWorker(); // Ensures the spare instance exists.
    idleWasmWorker = new Worker(assetSrc('engine_bundle.js'));

    const worker = this.worker;
    precompiledWasmModule.then((wasmModule) => {
      worker.postMessage({port: port1, useMemory64: USE_MEMORY64, wasmModule}, [
        port1,
      ]);
    });
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
