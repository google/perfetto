// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {PerfettoPlugin, PerfettoPluginStatic} from '../public/plugin';
import {createFakeTraceImpl} from './fake_trace_impl';
import {PluginAppInterface, PluginManagerImpl} from './plugin_manager';

const trace = createFakeTraceImpl();
const DummyApp: PluginAppInterface = {
  forkForPlugin: jest.fn(),
  trace,
};

const testPlugin = (pluginId: string): PerfettoPluginStatic<PerfettoPlugin> => {
    return (class {
        static id = pluginId;
    });
};

describe('PluginManagerImpl child manager', () => {
  test('child registry sees parent plugins but not vice versa', async () => {
    const parent = new PluginManagerImpl(DummyApp);
    const parentPlugin = testPlugin('test$parentPlugin');
    parent.registerPlugin(parentPlugin);
    parent.activatePlugins([parentPlugin.id]);
    await parent.onTraceLoad(trace);

    const child = parent.createChild();
    // Add to child, parent does not see it
    const childPlugin = testPlugin('test$childPlugin');
    child.registerPlugin(childPlugin);
    // Parent plug-in was already acviated in the parent manager
    child.activatePlugins([childPlugin.id]);
    await child.onTraceLoad(trace);

    expect(child.getPlugin(parentPlugin)).toBeDefined();
    expect(child.getPlugin(childPlugin)).toBeDefined();

    // The child plug-in is not registered, so an assertion throws
    expect(() => parent.getPlugin(childPlugin)).toThrow();
  });
});
