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

import {defer} from '../base/deferred';
import {assertExists, assertTrue} from '../base/logging';

// The 64-bit variant of TraceProcessor wasm is always built in all build
// configurations and we can depend on it from typescript.
import TraceProcessor64 from '../gen/trace_processor_memory64';

// The 32-bit variant may or may not be part of the build, depending on whether
// the user passes --only-wasm-memory64 to ui/build.js. When we are building
// also the 32-bit (e.g., in production builds) the import below will be
// redirected by rollup to '../gen/trace_processor' (The 32-bit module).
import TraceProcessor32 from './trace_processor_32_stub';

// For manual testing of the Memory32 build, we can disable the Memory64 check.
const DISABLE_MEMORY64_FOR_MANUAL_TEST = false;

// The Initialize() call will allocate a buffer of REQ_BUF_SIZE bytes which
// will be used to copy the input request data. This is to avoid passing the
// input data on the stack, which has a limited (~1MB) size.
// The buffer will be allocated by the C++ side and reachable at
// HEAPU8[reqBufferAddr, +REQ_BUFFER_SIZE].
const REQ_BUF_SIZE = 32 * 1024 * 1024;

// The end-to-end interaction between JS and Wasm is as follows:
// - [JS] Inbound data received by the worker (onmessage() in engine/index.ts).
//   - [JS] onRpcDataReceived() (this file)
//     - [C++] trace_processor_on_rpc_request (wasm_bridge.cc)
//       - [C++] some TraceProcessor::method()
//         for (batch in result_rows)
//           - [C++] RpcResponseFunction(bytes) (wasm_bridge.cc)
//             - [JS] onReply() (this file)
//               - [JS] postMessage() (this file)
export class WasmBridge {
  private aborted: boolean;
  private connection: TraceProcessor64.Module;
  private reqBufferAddr = 0;
  private lastStderr: string[] = [];
  private messagePort?: MessagePort;
  private useMemory64: boolean;

  constructor() {
    this.aborted = false;
    const deferredRuntimeInitialized = defer<void>();
    this.useMemory64 = hasMemory64Support();
    const initModule = this.useMemory64 ? TraceProcessor64 : TraceProcessor32;
    this.connection = initModule({
      locateFile: (s: string) => s,
      print: (line: string) => console.log(line),
      printErr: (line: string) => this.appendAndLogErr(line),
      onRuntimeInitialized: () => deferredRuntimeInitialized.resolve(),
    });

    deferredRuntimeInitialized.then(() => {
      const fn = this.connection.addFunction(this.onReply.bind(this), 'vpi');
      this.reqBufferAddr = this.wasmPtrCast(
        this.connection.ccall(
          'trace_processor_rpc_init',
          /* return=*/ 'pointer',
          /* args=*/ ['pointer', 'number'],
          [fn, REQ_BUF_SIZE],
        ),
      );
    });
  }

  initialize(port: MessagePort) {
    // Ensure that initialize() is called only once.
    assertTrue(this.messagePort === undefined);
    this.messagePort = port;
    // Note: setting .onmessage implicitly calls port.start() and dispatches the
    // queued messages. addEventListener('message') doesn't.
    this.messagePort.onmessage = this.onMessage.bind(this);
  }

  onMessage(msg: MessageEvent) {
    if (this.aborted) {
      throw new Error('Wasm module crashed');
    }
    assertTrue(msg.data instanceof Uint8Array);
    const data = msg.data as Uint8Array;
    let wrSize = 0;
    // If the request data is larger than our JS<>Wasm interop buffer, split it
    // into multiple writes. The RPC channel is byte-oriented and is designed to
    // deal with arbitrary fragmentations.
    while (wrSize < data.length) {
      const sliceLen = Math.min(data.length - wrSize, REQ_BUF_SIZE);
      const dataSlice = data.subarray(wrSize, wrSize + sliceLen);
      this.connection.HEAPU8.set(dataSlice, this.reqBufferAddr);
      wrSize += sliceLen;
      try {
        this.connection.ccall(
          'trace_processor_on_rpc_request', // C function name.
          'void', // Return type.
          ['number'], // Arg types.
          [sliceLen], // Args.
        );
      } catch (err) {
        this.aborted = true;
        let abortReason = `${err}`;
        if (err instanceof Error) {
          abortReason = `${err.name}: ${err.message}\n${err.stack}`;
        }
        abortReason += '\n\nstderr: \n' + this.lastStderr.join('\n');
        throw new Error(abortReason);
      }
    } // while(wrSize < data.length)
  }

  // This function is bound and passed to Initialize and is called by the C++
  // code while in the ccall(trace_processor_on_rpc_request).
  private onReply(heapPtrArg: bigint | number, size: number) {
    const heapPtr = this.wasmPtrCast(heapPtrArg);
    const data = this.connection.HEAPU8.slice(heapPtr, heapPtr + size);
    assertExists(this.messagePort).postMessage(data, [data.buffer]);
  }

  private appendAndLogErr(line: string) {
    console.warn(line);
    // Keep the last N lines in the |lastStderr| buffer.
    this.lastStderr.push(line);
    if (this.lastStderr.length > 512) {
      this.lastStderr.shift();
    }
  }

  // Takes a wasm pointer and converts it into a positive number < 2**53.
  // When using memory64 pointer args are passed as BigInt, but they are
  // guaranteed to be < 2**53 anyways.
  // When using memory32, pointer args are passed as numbers. However, because
  // they can be between 2GB and 4GB, we need to remove the negative sign.
  private wasmPtrCast(val: number | bigint): number {
    if (this.useMemory64) {
      return Number(val);
    }
    // Force heapPtr to be a positive using an unsigned right shift.
    // The issue here is the following: the matching code in wasm_bridge.cc
    // invokes this function passing  arguments as uint32_t. However, in the
    // wasm<>JS interop bindings, the uint32 args become Js numbers. If the
    // pointer is > 2GB, this number will be negative, which causes the wrong
    // behaviour when used as an offset on HEAP8U.
    assertTrue(typeof val === 'number');
    return Number(val) >>> 0; // static_cast<uint32_t>
  }
}

// Checks if the current environment supports Memory64.
function hasMemory64Support() {
  if (DISABLE_MEMORY64_FOR_MANUAL_TEST) {
    return false;
  }
  // Compiled version of WAT program `(module (memory i64 0))` to WASM.
  const memory64DetectProgram = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x05, 0x03, 0x01, 0x04,
    0x00, 0x00, 0x08, 0x04, 0x6e, 0x61, 0x6d, 0x65, 0x02, 0x01, 0x00,
  ]);
  try {
    new WebAssembly.Module(memory64DetectProgram);
    return true;
  } catch (e) {
    return false;
  }
}
