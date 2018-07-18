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

// tslint:disable:no-any

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
  private deferredHaveBlob: Deferred<void>;
  private deferredInitialized: Deferred<void>;
  private deferredReady: Deferred<void>;
  private fileReader: any;
  private blob: Blob|null;
  private callback: (_: WasmBridgeResponse) => void;
  private replyCount: number;
  private aborted: boolean;
  private outstandingRequests: Set<number>;

  connection: init_trace_processor.Module;

  constructor(
      init: init_trace_processor.InitWasm,
      callback: (_: WasmBridgeResponse) => void, fileReader: any) {
    this.replyCount = 0;
    this.deferredRuntimeInitialized = defer<void>();
    this.deferredHaveBlob = defer<void>();
    this.deferredInitialized = defer<void>();
    this.deferredReady = defer<void>();
    this.fileReader = fileReader;
    this.callback = callback;
    this.blob = null;
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

  onRead(offset: number, length: number, dstPtr: number): number {
    if (this.blob === null) {
      throw new Error('No blob set');
    }
    const slice = this.blob.slice(offset, offset + length);
    const buf: ArrayBuffer = this.fileReader.readAsArrayBuffer(slice);
    const buf8 = new Uint8Array(buf);
    this.connection.HEAPU8.set(buf8, dstPtr);
    return buf.byteLength;
  }

  onReply(reqId: number, success: boolean, heapPtr: number, size: number) {
    // The first reply (from Initialize) is special. It has no proto payload
    // and no associated callback.
    if (this.replyCount === 0) {
      this.replyCount++;
      this.deferredInitialized.resolve();
      return;
    }
    if (!this.outstandingRequests.has(reqId)) {
      throw new Error(`Unknown request id: "${reqId}"`);
    }
    this.outstandingRequests.delete(reqId);
    this.replyCount++;
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

  setBlob(blob: Blob) {
    if (this.blob !== null) throw new Error('Blob set twice.');
    this.blob = blob;
    this.deferredHaveBlob.resolve();
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
    await this.deferredHaveBlob;
    const readTraceFn =
        this.connection.addFunction(this.onRead.bind(this), 'iiii');
    const replyFn =
        this.connection.addFunction(this.onReply.bind(this), 'viiii');
    this.connection.ccall(
        'Initialize',
        'void',
        ['number', 'number', 'number'],
        [0, readTraceFn, replyFn]);
    await this.deferredInitialized;
    this.deferredReady.resolve();
  }
}
