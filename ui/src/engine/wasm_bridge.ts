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
import * as init_trace_processor from '../gen/trace_processor';

function writeToUIConsole(line: string) {
  console.log(line);
}

export interface WasmBridgeRequest {
  id: number;
  serviceName: string;
  methodName: string;
  data: Uint8Array;
}

export interface WasmBridgeResponse {
  id: number;
  success: boolean;
  data?: Uint8Array;
}

export class WasmBridge {
  // When this promise has resolved it is safe to call callWasm.
  whenInitialized: Promise<void>;

  private aborted: boolean;
  private currentRequestResult: WasmBridgeResponse|null;
  private connection: init_trace_processor.Module;

  constructor(init: init_trace_processor.InitWasm) {
    this.aborted = false;
    this.currentRequestResult = null;

    const deferredRuntimeInitialized = defer<void>();
    this.connection = init({
      locateFile: (s: string) => s,
      print: writeToUIConsole,
      printErr: writeToUIConsole,
      onRuntimeInitialized: () => deferredRuntimeInitialized.resolve(),
      onAbort: () => this.aborted = true,
    });
    this.whenInitialized = deferredRuntimeInitialized.then(() => {
      const fn = this.connection.addFunction(this.onReply.bind(this), 'viiii');
      this.connection.ccall('Initialize', 'void', ['number'], [fn]);
    });
  }

  callWasm(req: WasmBridgeRequest): WasmBridgeResponse {
    if (this.aborted) {
      return {
        id: req.id,
        success: false,
        data: undefined,
      };
    }
    // TODO(b/124805622): protoio can generate CamelCase names - normalize.
    const methodName = req.methodName;
    const name = methodName.charAt(0).toLowerCase() + methodName.slice(1);
    this.connection.ccall(
        `${req.serviceName}_${name}`,        // C method name.
        'void',                              // Return type.
        ['number', 'array', 'number'],       // Input args.
        [req.id, req.data, req.data.length]  // Args.
        );

    const result = assertExists(this.currentRequestResult);
    assertTrue(req.id === result.id);
    this.currentRequestResult = null;
    return result;
  }

  // This is invoked from ccall in the same call stack as callWasm.
  private onReply(
      reqId: number, success: boolean, heapPtr: number, size: number) {
    const data = this.connection.HEAPU8.slice(heapPtr, heapPtr + size);
    this.currentRequestResult = {
      id: reqId,
      success,
      data,
    };
  }
}
