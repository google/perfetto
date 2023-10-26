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

import {createEmptyState} from './empty_state';
import {Engine} from './engine';
import {PluginManager, PluginRegistry} from './plugins';
import {ViewerImpl} from './viewer';

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

const viewer = new ViewerImpl();
const engine = new FakeEngine();
globals.initStore(createEmptyState());

// We use `any` here to avoid checking possibly undefined types in tests.
let mockPlugin: any;
let manager: any;

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

  it('can activate plugin', () => {
    manager.activatePlugin('foo', viewer);

    expect(manager.isActive('foo')).toBe(true);
    expect(mockPlugin.onActivate).toHaveBeenCalledTimes(1);
  });

  it('can deactivate plugin', () => {
    manager.activatePlugin('foo', viewer);
    manager.deactivatePlugin('foo');

    expect(manager.isActive('foo')).toBe(false);
    expect(mockPlugin.onDeactivate).toHaveBeenCalledTimes(1);
  });

  it('invokes onTraceLoad when trace is loaded', () => {
    manager.activatePlugin('foo', viewer);
    manager.onTraceLoad(engine);

    expect(mockPlugin.onTraceLoad).toHaveBeenCalledTimes(1);
  });

  it('invokes onTraceLoad when plugin activated while trace loaded', () => {
    manager.onTraceLoad(engine);
    manager.activatePlugin('foo', viewer);

    expect(mockPlugin.onTraceLoad).toHaveBeenCalledTimes(1);
  });

  it('invokes onTraceUnload when plugin deactivated while trace loaded', () => {
    manager.activatePlugin('foo', viewer);
    manager.onTraceLoad(engine);
    manager.deactivatePlugin('foo');

    expect(mockPlugin.onTraceUnload).toHaveBeenCalledTimes(1);
  });
});
