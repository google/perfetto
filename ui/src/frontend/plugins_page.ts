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
import {PluginDescriptor} from '../public';
import {Button} from '../widgets/button';

import {createPage} from './pages';

export const PluginsPage = createPage({
  view() {
    return m(
        '.pf-plugins-page',
        m('h1', 'Plugins'),
        m(
            '.pf-plugins-topbar',
            m(Button, {
              minimal: false,
              label: 'Deactivate All',
              onclick: () => {
                for (const plugin of pluginRegistry.values()) {
                  pluginManager.deactivatePlugin(plugin.pluginId);
                }
                raf.scheduleFullRedraw();
              },
            }),
            m(Button, {
              minimal: false,
              label: 'Activate All',
              onclick: () => {
                for (const plugin of pluginRegistry.values()) {
                  pluginManager.activatePlugin(plugin.pluginId);
                }
                raf.scheduleFullRedraw();
              },
            }),
            ),
        m(
            '.pf-plugins-grid',
            Array.from(pluginRegistry.values()).map((plugin) => {
              return renderPluginRow(plugin);
            }),
            ));
  },
});

function renderPluginRow(plugin: PluginDescriptor): m.Children {
  const isActive = pluginManager.isActive(plugin.pluginId);
  return [
    plugin.pluginId,
    isActive ? m('.pf-tag.pf-active', 'Active') :
               m('.pf-tag.pf-inactive', 'Inactive'),
    m(Button, {
      label: isActive ? 'Deactivate' : 'Activate',
      onclick: () => {
        if (isActive) {
          pluginManager.deactivatePlugin(plugin.pluginId);
        } else {
          pluginManager.activatePlugin(plugin.pluginId);
        }
        raf.scheduleFullRedraw();
      },
    }),
  ];
}
