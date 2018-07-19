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

import {Module, ModuleArgs} from '../gen/trace_processor';

import {WasmBridge} from './wasm_bridge';

class MockModule implements Module {
  locateFile: (s: string) => string;
  onRuntimeInitialized: () => void;
  onAbort: () => void;
  addFunction = jest.fn();
  ccall = jest.fn();

  constructor() {
    this.locateFile = (_) => {
      throw new Error('locateFile not set');
    };
    this.onRuntimeInitialized = () => {
      throw new Error('onRuntimeInitialized not set');
    };
    this.onAbort = () => {
      throw new Error('onAbort not set');
    };
  }

  init(args: ModuleArgs): Module {
    this.locateFile = args.locateFile;
    this.onRuntimeInitialized = args.onRuntimeInitialized;
    this.onAbort = args.onAbort;
    return this;
  }

  get HEAPU8() {
    const heap = new Uint8Array(10);
    heap.set([0, 1, 2, 3, 4, 5, 6, 7, 8, 9], 0);
    return heap;
  }
}

test('wasm bridge should locate files', async () => {
  const m = new MockModule();
  const callback = jest.fn();
  const fileReader = jest.fn();
  const bridge = new WasmBridge(m.init.bind(m), callback, fileReader);
  expect(bridge);
  expect(m.locateFile('foo.wasm')).toBe('foo.wasm');
});

test('wasm bridge early calls are delayed', async () => {
  const m = new MockModule();
  const callback = jest.fn();
  const fileReader = jest.fn();
  const bridge = new WasmBridge(m.init.bind(m), callback, fileReader);

  const requestPromise = bridge.callWasm({
    id: 100,
    serviceName: 'service',
    methodName: 'method',
    data: new Uint8Array(42),
  });

  const readyPromise = bridge.initialize();

  m.onRuntimeInitialized();
  // tslint:disable-next-line no-any
  bridge.setBlob(null as any);
  bridge.onReply(0, true, 0, 0);

  await readyPromise;
  bridge.onReply(100, true, 0, 1);
  await requestPromise;
  expect(m.ccall.mock.calls[0][0]).toBe('Initialize');
  expect(m.ccall.mock.calls[1][0]).toBe('service_method');
  expect(callback.mock.calls[0][0].id).toBe(100);
});

test('wasm bridge aborts all calls on failure', async () => {
  const m = new MockModule();
  const callback = jest.fn();
  const fileReader = jest.fn();
  const bridge = new WasmBridge(m.init.bind(m), callback, fileReader);

  const readyPromise = bridge.initialize();

  m.onRuntimeInitialized();
  // tslint:disable-next-line no-any
  bridge.setBlob(null as any);
  bridge.onReply(0, true, 0, 0);

  bridge.callWasm({
    id: 100,
    serviceName: 'service',
    methodName: 'method',
    data: new Uint8Array(42),
  });

  await readyPromise;

  await bridge.callWasm({
    id: 200,
    serviceName: 'service',
    methodName: 'method',
    data: new Uint8Array(42),
  });

  bridge.onAbort();

  await bridge.callWasm({
    id: 300,
    serviceName: 'service',
    methodName: 'method',
    data: new Uint8Array(42),
  });

  expect(callback.mock.calls[0][0]).toEqual({
    id: 100,
    success: false,
    data: undefined,
  });
  expect(callback.mock.calls[1][0]).toEqual({
    id: 200,
    success: false,
    data: undefined,
  });
  expect(callback.mock.calls[2][0]).toEqual({
    id: 300,
    success: false,
    data: undefined,
  });
});
