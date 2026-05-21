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

// Manual types for the memory64 trace_processor emscripten module. Vite
// resolves '../virtual/trace_processor_memory64' to
// ui/src/gen/trace_processor_memory64.js at runtime (see
// pluginPerfettoVirtualWasmModules in ui/vite.config.mjs).

import * as WasmShape from './wasm_module_shape';

export = Wasm;

declare function Wasm(args: Wasm.ModuleArgs): Promise<Wasm.Module>;

declare namespace Wasm {
  export type FileSystemType = WasmShape.FileSystemType;
  export type FileSystemTypes = WasmShape.FileSystemTypes;
  export type FileSystemNode = WasmShape.FileSystemNode;
  export type FileSystem = WasmShape.FileSystem;
  export type Module = WasmShape.Module;
  export type ModuleArgs = WasmShape.ModuleArgs;
  export type InitWasm = WasmShape.InitWasm;
}
