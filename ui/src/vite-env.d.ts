// Copyright (C) 2024 The Android Open Source Project
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

/// <reference types="vite/client" />

// Lezer grammar files - compiled on-demand by vite-plugins/lezer-grammar.ts
declare module '*.grammar' {
  import {LRParser} from '@lezer/lr';
  export const parser: LRParser;
}

// WASM modules - served by vite-plugins/wasm-modules.ts
// Type definitions from gn/standalone/wasm_typescript_declaration.d.ts
declare namespace Wasm {
  interface FileSystemType {}
  interface FileSystemTypes {
    MEMFS: FileSystemType;
    IDBFS: FileSystemType;
    WORKERFS: FileSystemType;
  }
  interface FileSystemNode {
    contents: Uint8Array;
    usedBytes?: number;
  }
  interface FileSystem {
    mkdir(path: string, mode?: number): unknown;
    mount(type: FileSystemType, opts: unknown, mountpoint: string): unknown;
    unmount(mountpoint: string): void;
    unlink(mountpoint: string): void;
    lookupPath(path: string): {path: string; node: FileSystemNode};
    filesystems: FileSystemTypes;
  }
  interface Module {
    callMain(args: string[]): void;
    addFunction(f: unknown, argTypes: string): void;
    ccall(
      ident: string,
      returnType: string,
      argTypes: string[],
      args: unknown[],
    ): number;
    HEAPU8: Uint8Array;
    FS: FileSystem;
  }
  interface ModuleArgs {
    noInitialRun?: boolean;
    locateFile(s: string): string;
    print(s: string): void;
    printErr(s: string): void;
    onRuntimeInitialized(): void;
    onAbort?(): void;
    wasmBinary?: ArrayBuffer;
  }
}

type WasmFactory = (args: Wasm.ModuleArgs) => Wasm.Module;

declare module '../gen/trace_processor_memory64' {
  const factory: WasmFactory;
  export default factory;
}

declare module '../gen/proto_utils' {
  const factory: WasmFactory;
  export default factory;
}

declare module '../gen/traceconv' {
  const factory: WasmFactory;
  export default factory;
}
