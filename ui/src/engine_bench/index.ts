// Copyright (C) 2026 The Android Open Source Project
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

// Standalone worker entry for the engine startup benchmark. Drives the
// production WasmBridge and reports per-phase timings.

import {WasmBridge} from '../engine/wasm_bridge';

const tWorkerStart = performance.now();
const selfWorker = self as {} as Worker;

selfWorker.onmessage = (msg: MessageEvent) => {
  const data = msg.data as {
    useMemory64: boolean;
    wasmModule?: WebAssembly.Module;
  };

  const tBeforeBridge = performance.now();
  const bridge = new WasmBridge();
  const tAfterBridgeCtor = performance.now();

  const tBeforeStartInit = performance.now();
  bridge.startInit(data.useMemory64, data.wasmModule);
  const tAfterStartInit = performance.now();
  // We don't push RPCs through, so initialize() is intentionally skipped.

  bridge.whenInitialized().then(
    () => {
      const tDone = performance.now();
      selfWorker.postMessage({
        type: 'bench-marks',
        phases: {
          bundle_eval_ms: tBeforeBridge - tWorkerStart,
          bridge_ctor_ms: tAfterBridgeCtor - tBeforeBridge,
          start_init_sync_ms: tAfterStartInit - tBeforeStartInit,
          init_async_ms: tDone - tAfterStartInit,
          total_worker_ms: tDone - tWorkerStart,
        },
      });
    },
    (err) => {
      selfWorker.postMessage({
        type: 'bench-error',
        message: String((err && err.message) || err),
      });
    },
  );
};
