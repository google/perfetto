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

import TraceProcessor64 from '../gen/trace_processor_memory64';
import TraceProcessor32 from './trace_processor_32_stub';

export {TraceProcessor64, TraceProcessor32};

export const TRACE_PROCESSOR_64_WASM_URL = 'trace_processor_memory64.wasm';
export const TRACE_PROCESSOR_32_WASM_URL = 'trace_processor.wasm';
