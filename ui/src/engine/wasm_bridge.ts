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

import {ensureExists, assertTrue} from '../base/assert';
import {
  TraceProcessor32,
  TraceProcessor64,
  TraceProcessor64Threads,
  type WasmTier,
} from '../trace_processor/wasm_modules';

// Requests are copied in chunks no larger than REQ_BUF_SIZE. Each chunk gets
// an owned C++ allocation rather than using the stack (which is only ~1MB) or
// a shared reusable buffer (which would race the TP pthread).
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
  private aborted = false;
  private connection?: TraceProcessor64.Module;
  private reqBufferAddr = 0;
  private lastStderr: string[] = [];
  private messagePort?: MessagePort;
  private useMemory64 = false;

  // |precompiledModule| is compiled once on the main thread and shared with
  // every worker so V8 reuses the same tiered-up wasm code. The port's
  // onmessage is wired up only after init completes, so any RPC bytes that
  // arrive in the meantime stay queued on the port.
  async initialize(
    port: MessagePort,
    precompiledModule: WebAssembly.Module,
    tier: WasmTier,
  ): Promise<void> {
    assertTrue(this.messagePort === undefined);
    this.messagePort = port;
    this.useMemory64 = tier !== 'wasm32';

    const initModule =
      tier === 'memory64-threads'
        ? TraceProcessor64Threads
        : tier === 'memory64'
          ? TraceProcessor64
          : TraceProcessor32;
    const connection = await initModule({
      locateFile: (s: string) => s,
      print: (line: string) => console.log(line),
      printErr: (line: string) => this.appendAndLogErr(line),
      onRuntimeInitialized: () => {},
      instantiateWasm: (imports, successCallback) => {
        const instance = new WebAssembly.Instance(precompiledModule, imports);
        successCallback(instance, precompiledModule);
        return instance.exports;
      },
    });
    const fn = connection.addFunction(this.onReply.bind(this), 'vpi');
    connection.ccall(
      'trace_processor_rpc_init',
      /* return=*/ 'void',
      /* args=*/ ['pointer'],
      [fn],
    );
    this.connection = connection;

    // Setting .onmessage implicitly calls port.start() and flushes queued
    // messages. addEventListener('message') doesn't.
    port.onmessage = this.onMessage.bind(this);
  }

  onMessage(msg: MessageEvent) {
    if (this.aborted) {
      throw new Error('Wasm module crashed');
    }
    const connection = ensureExists(this.connection);
    assertTrue(msg.data instanceof Uint8Array);
    const data = msg.data as Uint8Array;
    let wrSize = 0;
    // If the request data is larger than our JS<>Wasm interop buffer, split it
    // into multiple writes. The RPC channel is byte-oriented and is designed to
    // deal with arbitrary fragmentations.
    while (wrSize < data.length) {
      const sliceLen = Math.min(data.length - wrSize, REQ_BUF_SIZE);
      const dataSlice = data.subarray(wrSize, wrSize + sliceLen);
      try {
        // Each message slice owns its handoff buffer. C++ releases it after
        // the responsive RPC frontend has copied the bytes into its framing
        // buffer/queue, so the TP pthread never races a reused JS buffer.
        this.reqBufferAddr = this.wasmPtrCast(
          connection.ccall(
            'trace_processor_rpc_alloc',
            /* return=*/ 'pointer',
            /* args=*/ ['number'],
            [sliceLen],
          ),
        );
        connection.HEAPU8.set(dataSlice, this.reqBufferAddr);
        wrSize += sliceLen;
        connection.ccall(
          'trace_processor_on_rpc_request', // C function name.
          'void', // Return type.
          ['pointer', 'number'], // Arg types.
          [this.reqBufferAddr, sliceLen], // Args.
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
    const data = ensureExists(this.connection).HEAPU8.slice(
      heapPtr,
      heapPtr + size,
    );
    ensureExists(this.messagePort).postMessage(data, [data.buffer]);
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
