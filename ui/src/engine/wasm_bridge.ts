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

import {defer, Deferred} from '../base/deferred';
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
  private deferredRuntimeInitialized: Deferred<void>;
  private deferredReady: Deferred<void>;
  private callback: (_: WasmBridgeResponse) => void;
  private aborted: boolean;
  private outstandingRequests: Set<number>;

  connection: init_trace_processor.Module;

  constructor(
      init: init_trace_processor.InitWasm,
      callback: (_: WasmBridgeResponse) => void) {
    this.deferredRuntimeInitialized = defer<void>();
    this.deferredReady = defer<void>();
    this.callback = callback;
    this.aborted = false;
    this.outstandingRequests = new Set();

    this.connection = init({
      locateFile: (s: string) => s,
      print: writeToUIConsole,
      printErr: writeToUIConsole,
      onRuntimeInitialized: () => this.deferredRuntimeInitialized.resolve(),
      onAbort: () => this.onAbort(),
    });
  }

  onAbort() {
    this.aborted = true;
    for (const id of this.outstandingRequests) {
      this.abortRequest(id);
    }
    this.outstandingRequests.clear();
  }

  onReply(reqId: number, success: boolean, heapPtr: number, size: number) {
    if (!this.outstandingRequests.has(reqId)) {
      throw new Error(`Unknown request id: "${reqId}"`);
    }
    this.outstandingRequests.delete(reqId);
    const data = this.connection.HEAPU8.slice(heapPtr, heapPtr + size);
    this.callback({
      id: reqId,
      success,
      data,
    });
  }

  abortRequest(requestId: number) {
    this.callback({
      id: requestId,
      success: false,
      data: undefined,
    });
  }

  async callWasm(req: WasmBridgeRequest): Promise<void> {
    await this.deferredReady;
    if (this.aborted) {
      this.abortRequest(req.id);
      return;
    }

    this.outstandingRequests.add(req.id);
    this.connection.ccall(
        `${req.serviceName}_${req.methodName}`,  // C method name.
        'void',                                  // Return type.
        ['number', 'array', 'number'],           // Input args.
        [req.id, req.data, req.data.length]      // Args.
        );
  }

  async initialize(): Promise<void> {
    await this.deferredRuntimeInitialized;
    const replyFn =
        this.connection.addFunction(this.onReply.bind(this), 'viiii');
    this.connection.ccall('Initialize', 'void', ['number'], [replyFn]);
    this.deferredReady.resolve();
  }
}
