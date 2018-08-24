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

import * as init_trace_processor from '../gen/trace_processor';

import {WasmBridge, WasmBridgeRequest} from './wasm_bridge';

// tslint:disable no-any

// We expect to get exactly one message from the creator of the worker:
// a MessagePort we should listen to for future messages.
// This indirection is due to workers not being able create workers in Chrome
// which is tracked at: crbug.com/31666
// TODO(hjd): Remove this once the fix has landed.
// Once we have the MessagePort we proxy all messages to WasmBridge#callWasm.
const anySelf = (self as any);
anySelf.onmessage = (msg: MessageEvent) => {
  const port: MessagePort = msg.data;

  const bridge =
      new WasmBridge(init_trace_processor, port.postMessage.bind(port));
  bridge.initialize();

  port.onmessage = (msg: MessageEvent) => {
    const request: WasmBridgeRequest = msg.data;
    bridge.callWasm(request);
  };
};
