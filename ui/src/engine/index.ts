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

import {WasmBridge} from './wasm_bridge';
import memory64, {InitWasm} from '../gen/trace_processor_memory64';
import {base64Decode} from '../base/string_utils';

const selfWorker = self as {} as Worker;

// For manual testing of the Memory32 build, we can disable the Memory64 check.
const DISABLE_MEMORY64_FOR_MANUAL_TEST = false;

// Checks if the current environment supports Memory64.
async function hasMemory64Support() {
  if (DISABLE_MEMORY64_FOR_MANUAL_TEST) {
    return false;
  }
  try {
    // Compiled version of WAT program `(module (memory i64 0))` to WASM.
    await WebAssembly.compile(base64Decode('AGFzbQEAAAAFAwEEAAAIBG5hbWUCAQA='));
    return true;
  } catch (e) {
    return false;
  }
}

// This variable will be replaced by rollup at build time based on the
// existence of gen/trace_processor; we don't unconditionally build that.
declare const __IS_MEMORY64_ONLY__: string | undefined;

async function createTraceProcessor(): Promise<InitWasm> {
  if (await hasMemory64Support()) {
    return memory64;
  }
  // See comment on __IS_MEMORY64_ONLY__ above.
  if (!__IS_MEMORY64_ONLY__) {
    // @ts-ignore: TS2307. This module is optional and may not exist. Rollup
    // correctly ensures this code is not included in the final bundle.
    return [(await import('../gen/trace_processor')).default, '32'];
  }
  throw new Error(
    `Unable to load trace processor: running a browser without Memory64 ` +
      `support and with no memory32 build.`,
  );
}

async function init() {
  const wasmBridge = new WasmBridge(await createTraceProcessor());

  // There are two message handlers here:
  // 1. The Worker (self.onmessage) handler.
  // 2. The MessagePort handler.
  // The sequence of actions is the following:
  // 1. The frontend does one postMessage({port: MessagePort}) on the Worker
  //    scope. This message transfers the MessagePort.
  //    This is the only postMessage we'll ever receive here.
  // 2. All the other messages (i.e. the TraceProcessor RPC binary pipe) will be
  //    received on the MessagePort.

  // Receives the boostrap message from the frontend with the MessagePort.
  selfWorker.onmessage = (msg: MessageEvent) => {
    const port = msg.data as MessagePort;
    wasmBridge.initialize(port);
  };
}

init();
