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

import {Plugin, PluginContext} from '../public';

import {PluginManager, PluginRegistry} from './plugins';
import {ViewerImpl} from './viewer';

const viewer = new ViewerImpl();

class FooPlugin implements Plugin {
  onActivate(_: PluginContext): void {}
}

test('can activate plugin', () => {
  const registry = new PluginRegistry();
  registry.register({
    pluginId: 'foo',
    plugin: FooPlugin,
  });
  const manager = new PluginManager(registry);
  manager.activatePlugin('foo', viewer);
  expect(manager.isActive('foo')).toBe(true);
});

test('can deactivate plugin', () => {
  const registry = new PluginRegistry();
  registry.register({
    pluginId: 'foo',
    plugin: FooPlugin,
  });
  const manager = new PluginManager(registry);
  manager.activatePlugin('foo', viewer);
  manager.deactivatePlugin('foo');
  expect(manager.isActive('foo')).toBe(false);
});
