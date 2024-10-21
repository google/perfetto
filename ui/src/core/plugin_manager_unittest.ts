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

import {PerfettoPlugin} from '../public/plugin';
import {AppImpl} from './app_impl';
import {
  createFakeTraceImpl,
  initializeAppImplForTesting,
} from './fake_trace_impl';
import {PluginManager} from './plugin_manager';

function makeMockPlugin(): PerfettoPlugin {
  return {
    onActivate: jest.fn(),
    onTraceLoad: jest.fn(),
    onTraceUnload: jest.fn(),
  };
}

let mockPlugin: PerfettoPlugin;
let manager: PluginManager;

describe('PluginManger', () => {
  beforeEach(() => {
    mockPlugin = makeMockPlugin();
    initializeAppImplForTesting();
    manager = new PluginManager(initializeAppImplForTesting());
    manager.registerPlugin({
      pluginId: 'foo',
      plugin: mockPlugin,
    });
  });

  it('can activate plugin', async () => {
    await manager.activatePlugin('foo');

    expect(manager.isActive('foo')).toBe(true);
    expect(mockPlugin.onActivate).toHaveBeenCalledTimes(1);
  });

  it('invokes onTraceLoad when trace is loaded', async () => {
    const trace = createFakeTraceImpl();
    AppImpl.instance.setActiveTrace(trace);
    await manager.onTraceLoad(trace);
    await manager.activatePlugin('foo');

    expect(mockPlugin.onTraceLoad).toHaveBeenCalledTimes(1);
  });

  it('invokes onTraceLoad when plugin activated while trace loaded', async () => {
    const trace = createFakeTraceImpl();
    AppImpl.instance.setActiveTrace(trace);
    await manager.onTraceLoad(trace);
    await manager.activatePlugin('foo');

    expect(mockPlugin.onTraceLoad).toHaveBeenCalledTimes(1);
  });
});
