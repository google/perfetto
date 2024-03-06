// Copyright (C) 2023 The Android Open Source Project
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

import {globals} from '../frontend/globals';
import {Plugin} from '../public';
import {Engine} from '../trace_processor/engine';

import {createEmptyState} from './empty_state';
import {PluginManager, PluginRegistry} from './plugins';

class FakeEngine extends Engine {
  id: string = 'TestEngine';

  rpcSendRequestBytes(_data: Uint8Array) {}
}

function makeMockPlugin(): Plugin {
  return {
    onActivate: jest.fn(),
    onDeactivate: jest.fn(),
    onTraceLoad: jest.fn(),
    onTraceUnload: jest.fn(),
  };
}

const engine = new FakeEngine();
globals.initStore(createEmptyState());

let mockPlugin: Plugin;
let manager: PluginManager;

describe('PluginManger', () => {
  beforeEach(() => {
    mockPlugin = makeMockPlugin();
    const registry = new PluginRegistry();
    registry.register({
      pluginId: 'foo',
      plugin: mockPlugin,
    });
    manager = new PluginManager(registry);
  });

  it('can activate plugin', async () => {
    await manager.activatePlugin('foo');

    expect(manager.isActive('foo')).toBe(true);
    expect(mockPlugin.onActivate).toHaveBeenCalledTimes(1);
  });

  it('can deactivate plugin', async () => {
    await manager.activatePlugin('foo');
    await manager.deactivatePlugin('foo');

    expect(manager.isActive('foo')).toBe(false);
    expect(mockPlugin.onDeactivate).toHaveBeenCalledTimes(1);
  });

  it('invokes onTraceLoad when trace is loaded', async () => {
    await manager.activatePlugin('foo');
    await manager.onTraceLoad(engine);

    expect(mockPlugin.onTraceLoad).toHaveBeenCalledTimes(1);
  });

  it('invokes onTraceLoad when plugin activated while trace loaded', async () => {
    await manager.onTraceLoad(engine);
    await manager.activatePlugin('foo');

    expect(mockPlugin.onTraceLoad).toHaveBeenCalledTimes(1);
  });

  it('invokes onTraceUnload when plugin deactivated while trace loaded', async () => {
    await manager.activatePlugin('foo');
    await manager.onTraceLoad(engine);
    await manager.deactivatePlugin('foo');

    expect(mockPlugin.onTraceUnload).toHaveBeenCalledTimes(1);
  });
});
