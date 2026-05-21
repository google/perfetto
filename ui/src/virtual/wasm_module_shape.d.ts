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

// Shared shape for the hand-curated emscripten wasm glue modules
// (trace_processor, trace_processor_memory64, proto_utils, traceconv). All
// four expose the same surface; the virtual .d.ts files re-export this
// namespace under module-specific names so callers can write e.g.
// `TraceProcessor64.Module` and `WasmModuleGen.ModuleArgs`.

export interface FileSystemType {}

export interface FileSystemTypes {
  MEMFS: FileSystemType;
  IDBFS: FileSystemType;
  WORKERFS: FileSystemType;
}

export interface FileSystemNode {
  contents: Uint8Array;
  usedBytes?: number;
}

export interface FileSystem {
  mkdir(path: string, mode?: number): unknown;
  mount(type: FileSystemType, opts: unknown, mountpoint: string): unknown;
  unmount(mountpoint: string): void;
  unlink(mountpoint: string): void;
  lookupPath(path: string): {path: string; node: FileSystemNode};
  filesystems: FileSystemTypes;
}

export interface Module {
  callMain(args: string[]): void;
  addFunction(f: unknown, argTypes: string): number;
  ccall(
    ident: string,
    returnType: string,
    argTypes: string[],
    args: unknown[],
  ): number;
  HEAPU8: Uint8Array;
  FS: FileSystem;
}

export interface ModuleArgs {
  noInitialRun?: boolean;
  locateFile(s: string): string;
  print(s: string): void;
  printErr(s: string): void;
  onRuntimeInitialized(): void;
  onAbort?(): void;
  wasmBinary?: ArrayBuffer;
  // Optional emscripten hook. When provided, the loader skips its own
  // fetch + compile and invokes this callback to obtain the instance.
  instantiateWasm?(
    imports: WebAssembly.Imports,
    successCallback: (
      instance: WebAssembly.Instance,
      mod: WebAssembly.Module,
    ) => void,
  ): WebAssembly.Exports;
}

export interface InitWasm {
  (_: ModuleArgs): Promise<Module>;
}
