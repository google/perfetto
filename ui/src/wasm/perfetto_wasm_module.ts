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

// Shared ambient type for the four emscripten JS glue files in ui/src/gen/
// (trace_processor[_memory64], traceconv, proto_utils). Each is an ES module
// (MODULARIZE=1 + EXPORT_ES6=1) whose default export is an
// EmscriptenModuleFactory<PerfettoWasmModule>. The per-file .d.ts shims
// alongside each .js (written by ui/build.mjs) reference this type.
//
// @types/emscripten only declares the base EmscriptenModule shape; our wasm
// targets opt extra runtime methods in via EXPORTED_RUNTIME_METHODS (see
// gn/standalone/wasm.gni). Declare those here so call sites get them typed
// without per-site casts.

/// <reference types="emscripten" />

// Extra FS surface that emcc exposes when filesystem libs (workerfs etc.)
// are linked but @types/emscripten doesn't model.
interface PerfettoFs {
  filesystems: {
    MEMFS: Emscripten.FileSystemType;
    IDBFS: Emscripten.FileSystemType;
    WORKERFS: Emscripten.FileSystemType;
  };
}

export interface PerfettoWasmModule extends EmscriptenModule {
  ccall: typeof ccall;
  callMain(args: string[]): void;
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  addFunction(fn: (...args: any[]) => any, sig: string): number;
  FS: typeof FS & PerfettoFs;
}
