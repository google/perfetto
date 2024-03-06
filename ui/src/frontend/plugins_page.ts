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

import m from 'mithril';

import {pluginManager, pluginRegistry} from '../common/plugins';
import {raf} from '../core/raf_scheduler';
import {Button} from '../widgets/button';

import {exists} from '../base/utils';
import {PluginDescriptor} from '../public';
import {createPage} from './pages';
import {defaultPlugins} from '../common/default_plugins';

export const PluginsPage = createPage({
  view() {
    return m(
      '.pf-plugins-page',
      m('h1', 'Plugins'),
      m(
        '.pf-plugins-topbar',
        m(Button, {
          minimal: false,
          label: 'Disable All',
          onclick: async () => {
            for (const plugin of pluginRegistry.values()) {
              await pluginManager.disablePlugin(plugin.pluginId, true);
              raf.scheduleFullRedraw();
            }
          },
        }),
        m(Button, {
          minimal: false,
          label: 'Enable All',
          onclick: async () => {
            for (const plugin of pluginRegistry.values()) {
              await pluginManager.enablePlugin(plugin.pluginId, true);
              raf.scheduleFullRedraw();
            }
          },
        }),
        m(Button, {
          minimal: false,
          label: 'Restore Defaults',
          onclick: async () => {
            await pluginManager.restoreDefaults(true);
            raf.scheduleFullRedraw();
          },
        }),
      ),
      m(
        '.pf-plugins-grid',
        [
          m('span', 'Plugin'),
          m('span', 'Default?'),
          m('span', 'Enabled?'),
          m('span', 'Active?'),
          m('span', 'Control'),
          m('span', 'Load Time'),
        ],
        Array.from(pluginRegistry.values()).map((plugin) => {
          return renderPluginRow(plugin);
        }),
      ));
  },
});

function renderPluginRow(plugin: PluginDescriptor): m.Children {
  const pluginId = plugin.pluginId;
  const isDefault = defaultPlugins.includes(pluginId);
  const pluginDetails = pluginManager.plugins.get(pluginId);
  const isActive = pluginManager.isActive(pluginId);
  const isEnabled = pluginManager.isEnabled(pluginId);
  const loadTime = pluginDetails?.previousOnTraceLoadTimeMillis;
  return [
    m('span', pluginId),
    m('span', isDefault ? 'Yes' : 'No'),
    isEnabled ? m('.pf-tag.pf-active', 'Enabled') :
      m('.pf-tag.pf-inactive', 'Disabled'),
    isActive ? m('.pf-tag.pf-active', 'Active') :
      m('.pf-tag.pf-inactive', 'Inactive'),
    m(Button, {
      label: isActive ? 'Disable' : 'Enable',
      onclick: async () => {
        if (isActive) {
          await pluginManager.disablePlugin(pluginId, true);
        } else {
          await pluginManager.enablePlugin(pluginId, true);
        }
        raf.scheduleFullRedraw();
      },
    }),
    exists(loadTime) ?
      m('span', `${loadTime.toFixed(1)} ms`) :
      m('span', `-`),
  ];
}
