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
import {raf} from '../core/raf_scheduler';
import {Button} from '../widgets/button';
import {exists} from '../base/utils';
import {PluginDescriptor} from '../public/plugin';
import {defaultPlugins} from '../core/default_plugins';
import {Intent} from '../widgets/common';
import {PageAttrs} from '../core/router';
import {AppImpl} from '../core/app_impl';

export class PluginsPage implements m.ClassComponent<PageAttrs> {
  view() {
    const pluginManager = AppImpl.instance.plugins;
    const registeredPlugins = pluginManager.getRegisteredPlugins();
    return m(
      '.pf-plugins-page',
      m('h1', 'Plugins'),
      pluginManager.needsRestart &&
        m(
          'h3.restart_needed',
          'Some plugins have been disabled. ' +
            'Please reload your page to apply the changes.',
        ),
      m(
        '.pf-plugins-topbar',
        m(Button, {
          intent: Intent.Primary,
          label: 'Disable All',
          onclick: async () => {
            for (const plugin of registeredPlugins) {
              await pluginManager.disablePlugin(plugin.pluginId);
              raf.scheduleFullRedraw();
            }
          },
        }),
        m(Button, {
          intent: Intent.Primary,
          label: 'Enable All',
          onclick: async () => {
            for (const plugin of registeredPlugins) {
              await pluginManager.enablePlugin(plugin.pluginId);
              raf.scheduleFullRedraw();
            }
          },
        }),
        m(Button, {
          intent: Intent.Primary,
          label: 'Restore Defaults',
          onclick: async () => {
            await pluginManager.restoreDefaults();
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
        registeredPlugins.map((plugin) => renderPluginRow(plugin)),
      ),
    );
  }
}

function renderPluginRow(plugin: PluginDescriptor): m.Children {
  const pluginManager = AppImpl.instance.plugins;
  const pluginId = plugin.pluginId;
  const isDefault = defaultPlugins.includes(pluginId);
  const pluginDetails = pluginManager.plugins.get(pluginId);
  const isActive = pluginManager.isActive(pluginId);
  const isEnabled = pluginManager.isEnabled(pluginId);
  const loadTime = pluginDetails?.previousOnTraceLoadTimeMillis;
  return [
    m('span', pluginId),
    m('span', isDefault ? 'Yes' : 'No'),
    isEnabled
      ? m('.pf-tag.pf-active', 'Enabled')
      : m('.pf-tag.pf-inactive', 'Disabled'),
    isActive
      ? m('.pf-tag.pf-active', 'Active')
      : m('.pf-tag.pf-inactive', 'Inactive'),
    m(Button, {
      label: isEnabled ? 'Disable' : 'Enable',
      intent: Intent.Primary,
      onclick: async () => {
        if (isEnabled) {
          await pluginManager.disablePlugin(pluginId);
        } else {
          await pluginManager.enablePlugin(pluginId);
        }
        raf.scheduleFullRedraw();
      },
    }),
    exists(loadTime) ? m('span', `${loadTime.toFixed(1)} ms`) : m('span', `-`),
  ];
}
