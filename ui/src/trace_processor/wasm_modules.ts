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

// Single source of truth pairing each trace_processor emscripten module
// with its .wasm asset filename. Both names come from the same wasm_lib()
// GN target — keeping them on adjacent lines here makes it impossible to
// rename one without updating the other.
//
// Imported by both the main thread (wasm_engine_proxy.ts, which fetches
// and precompiles the .wasm) and the worker (wasm_bridge.ts, which calls
// the module factory).
//
// '../virtual/trace_processor[_memory64]' is resolved by Vite to the real
// emscripten glue under ui/src/gen at runtime (see
// pluginPerfettoVirtualWasmModules in vite.config.mjs). TypeScript sees only
// the manually-curated .d.ts in ui/src/virtual/. Under --only-wasm-memory64
// the 32-bit module is redirected to the throwing stub
// ui/src/virtual/trace_processor.js.

import {assetSrc} from '../base/assets';
import TraceProcessor32 from '../virtual/trace_processor';
import TraceProcessor64 from '../virtual/trace_processor_memory64';

export {TraceProcessor64, TraceProcessor32};

let memory64SupportCache: boolean | undefined;

// Whether the current environment supports the WebAssembly memory64 proposal.
// The main thread calls this to pick the .wasm to fetch; the worker calls it
// to pick the matching factory — both run the same check against the same JS
// engine, so they agree without needing to forward a flag.
export function memory64Supported(): boolean {
  if (memory64SupportCache !== undefined) return memory64SupportCache;
  // Compiled bytes for `(module (memory i64 0))`.
  const program = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x05, 0x03, 0x01, 0x04,
    0x00, 0x00, 0x08, 0x04, 0x6e, 0x61, 0x6d, 0x65, 0x02, 0x01, 0x00,
  ]);
  try {
    new WebAssembly.Module(program);
    return (memory64SupportCache = true);
  } catch {
    return (memory64SupportCache = false);
  }
}

// Resolved URL of the .wasm matching the factory we'd pick for this env.
export function traceProcessorWasmUrl(): string {
  return assetSrc(
    memory64Supported()
      ? 'trace_processor_memory64.wasm'
      : 'trace_processor.wasm',
  );
}
