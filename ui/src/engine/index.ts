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

import {assertExists} from '../base/logging';
import {EngineWorkerInitMessage} from '../common/worker_messages';
import {WasmBridge} from './wasm_bridge';

const selfWorker = self as {} as Worker;
const wasmBridge = new WasmBridge();

// There are two message handlers here:
// 1. The Worker (self.onmessage) handler.
// 2. The MessagePort handler.
// When the app bootstraps, frontend/index.ts creates a MessageChannel and sends
// one end to the controller (the other worker) and the other end to us, so that
// the controller can interact with the Wasm worker without roundtrips through
// the frontend.
// The sequence of actions is the following:
// 1. The frontend does one postMessage({port: MessagePort}) on the Worker
//    scope. This message transfers the MessagePort (whose other end is
//    connected to the Conotroller). This is the only postMessage we'll ever
//    receive here.
// 2. All the other messages (i.e. the TraceProcessor RPC binary pipe) will be
//    received on the MessagePort.

// Receives the boostrap message from the frontend with the MessagePort.
selfWorker.onmessage = (msg: MessageEvent) => {
  const port = assertExists((msg.data as EngineWorkerInitMessage).enginePort);
  wasmBridge.initialize(port);
};
