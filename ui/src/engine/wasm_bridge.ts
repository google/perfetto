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
import * as initTraceProcessor from '../gen/trace_processor';

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
  // When this promise has resolved it is safe to call callWasm.
  whenInitialized: Promise<void>;

  private aborted: boolean;
  private connection: initTraceProcessor.Module;
  private reqBufferAddr = 0;
  private lastStderr: string[] = [];
  private messagePort?: MessagePort;

  constructor() {
    this.aborted = false;
    const deferredRuntimeInitialized = defer<void>();
    this.connection = initTraceProcessor({
      locateFile: (s: string) => s,
      print: (line: string) => console.log(line),
      printErr: (line: string) => this.appendAndLogErr(line),
      onRuntimeInitialized: () => deferredRuntimeInitialized.resolve(),
    });
    this.whenInitialized = deferredRuntimeInitialized.then(() => {
      const fn = this.connection.addFunction(this.onReply.bind(this), 'vii');
      this.reqBufferAddr = this.connection.ccall(
          'trace_processor_rpc_init',
          /* return=*/ 'number',
          /* args=*/['number', 'number'],
          [fn, REQ_BUF_SIZE]);
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
            'trace_processor_on_rpc_request',  // C function name.
            'void',                            // Return type.
            ['number'],                        // Arg types.
            [sliceLen],                        // Args.
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
    }  // while(wrSize < data.length)
  }

  // This function is bound and passed to Initialize and is called by the C++
  // code while in the ccall(trace_processor_on_rpc_request).
  private onReply(heapPtr: number, size: number) {
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
}
