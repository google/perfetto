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
// trace_processor_32_stub is rewritten by vite to '../gen/trace_processor'
// in normal builds (see vite.config.mjs); under --only-wasm-memory64 the
// alias is dropped and the local file is a throwing stub.

import {assetSrc} from '../base/assets';
import TraceProcessor64 from '../gen/trace_processor_memory64';
import TraceProcessor64Threads from '../gen/trace_processor_memory64_threads';
import TraceProcessor32 from './trace_processor_32_stub';

export {TraceProcessor64, TraceProcessor64Threads, TraceProcessor32};

export type WasmTier = 'memory64-threads' | 'memory64' | 'wasm32';

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

// Threads are useful only in combination with memory64 for the cancellation
// tier. Constructing shared memory performs a combined feature check rather
// than assuming support from the two individual proposals.
export function memory64ThreadsSupported(): boolean {
  if (!globalThis.crossOriginIsolated || !memory64Supported()) return false;
  if (typeof globalThis.SharedArrayBuffer === 'undefined') return false;
  try {
    new WebAssembly.Memory({
      initial: 1,
      maximum: 1,
      shared: true,
      index: 'i64',
    } as WebAssembly.MemoryDescriptor);
    return true;
  } catch {
    return false;
  }
}

export function traceProcessorWasmTier(): WasmTier {
  if (memory64ThreadsSupported()) return 'memory64-threads';
  if (memory64Supported()) return 'memory64';
  return 'wasm32';
}

// Resolved URL of the .wasm matching the factory we'd pick for this env.
export function traceProcessorWasmUrl(): string {
  switch (traceProcessorWasmTier()) {
    case 'memory64-threads':
      return assetSrc('trace_processor_memory64_threads.wasm');
    case 'memory64':
      return assetSrc('trace_processor_memory64.wasm');
    case 'wasm32':
      return assetSrc('trace_processor.wasm');
  }
}
