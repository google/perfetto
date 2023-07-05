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

import {PluginContext} from '../public';

import {PluginManager, PluginRegistry} from './plugins';

test('can activate plugin', () => {
  const registry = new PluginRegistry();
  registry.register({
    pluginId: 'foo',
    activate: (_: PluginContext) => {},
  });
  const manager = new PluginManager(registry);
  manager.activatePlugin('foo');
  expect(manager.isActive('foo')).toBe(true);
});

test('can deactivate plugin', () => {
  const registry = new PluginRegistry();
  registry.register({
    pluginId: 'foo',
    activate: (_: PluginContext) => {},
  });
  const manager = new PluginManager(registry);
  manager.activatePlugin('foo');
  manager.deactivatePlugin('foo');
  expect(manager.isActive('foo')).toBe(false);
});
