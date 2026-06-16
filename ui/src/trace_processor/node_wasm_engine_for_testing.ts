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

// A minimal EngineBase implementation that hosts the trace_processor WASM
// module directly in the current (Node) process — no Web Worker, no
// MessagePort. Intended only for vitest unit tests that want to run real
// PerfettoSQL queries against a real engine.
//
// The browser path (wasm_engine_proxy.ts + engine/wasm_bridge.ts) keeps the
// WASM on a worker and bridges I/O over a MessagePort for thread isolation.
// Here we drop that layer: the JS<>WASM RPC is synchronous, so we copy request
// bytes into the module's HEAPU8 and read the reply back inline via the same
// addFunction() callback the bridge uses.
//
// We target the 32-bit module so this works on Node versions whose V8 does not
// support the memory64 proposal. wasm_modules' TraceProcessor32 resolves via the
// trace_processor_32_stub indirection; vitest.config.mjs aliases that stub to
// the real ../gen/trace_processor module (matching the production vite alias).

import * as fs from 'fs';
import * as path from 'path';

// Type-only: the 32-bit stub is typed `never`, so we borrow the (identical)
// factory type from the memory64 declarations for the module/return types.
import type initTraceProcessorMem64 from '../gen/trace_processor_memory64';
import {TraceProcessor32} from './wasm_modules';
import {EngineBase} from './engine';

type InitTraceProcessor = typeof initTraceProcessorMem64;

// Must match REQ_BUF_SIZE in engine/wasm_bridge.ts: the C++ side allocates a
// buffer of this size for incoming request bytes.
const REQ_BUF_SIZE = 32 * 1024 * 1024;

// Converts a 32-bit wasm pointer into a positive number usable as a HEAPU8
// offset. memory32 pointers come across as JS numbers, but pointers above 2GB
// arrive negative (the uint32 is reinterpreted as int32), so force unsigned.
// See WasmBridge.wasmPtrCast in engine/wasm_bridge.ts.
function wasmPtr32(val: number): number {
  return val >>> 0;
}

type WasmModule = Awaited<ReturnType<InitTraceProcessor>>;

// Resolves the path to the 32-bit trace_processor.wasm. Assumes it has been
// built; if not, reading it later will fail loudly. The gen trace_processor.js
// factory is a build artifact whose real location is <outDir>/ui/tsc/gen; the
// matching .wasm lives in <outDir>/wasm.
export function locateTraceProcessorWasm(): string {
  // cwd differs by entry point: ui/ when running vitest directly (src/gen is a
  // symlink into the build output), or <outDir> under ui/run-unittests (gen
  // lives at ui/tsc/gen). Resolve whichever exists, then derive the sibling.
  const realGenJs = [
    path.resolve(process.cwd(), 'src/gen/trace_processor.js'),
    path.resolve(process.cwd(), 'ui/tsc/gen/trace_processor.js'),
  ].reduce<string | undefined>((found, p) => {
    if (found) return found;
    try {
      return fs.realpathSync(p);
    } catch {
      return undefined;
    }
  }, undefined);
  if (!realGenJs) {
    throw new Error('trace_processor.js not found; build the UI wasm first.');
  }
  // <outDir>/ui/tsc/gen/*.js -> <outDir>/wasm/trace_processor.wasm
  return path.resolve(
    path.dirname(realGenJs),
    '../../../wasm/trace_processor.wasm',
  );
}

export class NodeWasmEngine extends EngineBase implements Disposable {
  readonly mode = 'WASM';
  readonly id = 'node-wasm';
  private module!: WasmModule;
  private reqBufferAddr = 0;

  private constructor() {
    super();
  }

  static async create(wasmPath: string): Promise<NodeWasmEngine> {
    const engine = new NodeWasmEngine();
    await engine.init(wasmPath);
    return engine;
  }

  private async init(wasmPath: string): Promise<void> {
    const file = fs.readFileSync(wasmPath);
    // Hand the bytes directly to Emscripten so it doesn't try to fetch() them.
    const wasmBinary = file.buffer.slice(
      file.byteOffset,
      file.byteOffset + file.byteLength,
    ) as ArrayBuffer;
    // TraceProcessor32 is the vite-aliased real 32-bit factory at runtime, but
    // typed `never` (the stub); cast to the shared factory type.
    const init = TraceProcessor32 as unknown as InitTraceProcessor;
    const module = await init({
      locateFile: (s: string) => s,
      print: (line: string) => console.log(line),
      printErr: (line: string) => console.warn(line),
      onRuntimeInitialized: () => {},
      wasmBinary,
    });
    const fn = module.addFunction(this.onReply.bind(this), 'vpi');
    this.reqBufferAddr = wasmPtr32(
      module.ccall(
        'trace_processor_rpc_init',
        /* return=*/ 'pointer',
        /* args=*/ ['pointer', 'number'],
        [fn, REQ_BUF_SIZE],
      ),
    );
    this.module = module;
  }

  // Called by the C++ code (while inside trace_processor_on_rpc_request) with a
  // pointer/size into HEAPU8 holding the response bytes.
  private onReply(heapPtr: number, size: number) {
    const addr = wasmPtr32(heapPtr);
    const data = this.module.HEAPU8.slice(addr, addr + size);
    super.onRpcResponseBytes(data);
  }

  rpcSendRequestBytes(data: Uint8Array): void {
    // Mirrors WasmBridge.onMessage: split into REQ_BUF_SIZE-sized writes since
    // the RPC channel is byte-oriented and handles arbitrary fragmentation.
    let wrSize = 0;
    while (wrSize < data.length) {
      const sliceLen = Math.min(data.length - wrSize, REQ_BUF_SIZE);
      this.module.HEAPU8.set(
        data.subarray(wrSize, wrSize + sliceLen),
        this.reqBufferAddr,
      );
      wrSize += sliceLen;
      this.module.ccall(
        'trace_processor_on_rpc_request',
        'void',
        ['number'],
        [sliceLen],
      );
    }
  }

  [Symbol.dispose]() {}
}
