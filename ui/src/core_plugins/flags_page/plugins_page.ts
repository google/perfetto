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
import {Button} from '../../widgets/button';
import {exists} from '../../base/utils';
import {defaultPlugins} from '../../core/default_plugins';
import {Intent} from '../../widgets/common';
import {PageAttrs} from '../../public/page';
import {AppImpl} from '../../core/app_impl';
import {PluginWrapper} from '../../core/plugin_manager';
import {raf} from '../../core/raf_scheduler';

// This flag indicated whether we need to restart the UI to apply plugin
// changes. It is purposely a global as we want it to outlive the Mithril
// component, and it'll be reset we restart anyway.
let needsRestart = false;

export class PluginsPage implements m.ClassComponent<PageAttrs> {
  view() {
    const pluginManager = AppImpl.instance.plugins;
    const registeredPlugins = pluginManager.getAllPlugins();
    return m(
      '.pf-plugins-page',
      m('h1', 'Plugins'),
      needsRestart &&
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
              plugin.enableFlag.set(false);
            }
            needsRestart = true;
            raf.scheduleFullRedraw();
          },
        }),
        m(Button, {
          intent: Intent.Primary,
          label: 'Enable All',
          onclick: async () => {
            for (const plugin of registeredPlugins) {
              plugin.enableFlag.set(true);
            }
            needsRestart = true;
            raf.scheduleFullRedraw();
          },
        }),
        m(Button, {
          intent: Intent.Primary,
          label: 'Restore Defaults',
          onclick: async () => {
            for (const plugin of registeredPlugins) {
              plugin.enableFlag.reset();
            }
            needsRestart = true;
            raf.scheduleFullRedraw();
          },
        }),
      ),
      m(
        '.pf-plugins-grid',
        m('span', 'Plugin'),
        m('span', 'Default?'),
        m('span', 'Enabled?'),
        m('span', 'Active?'),
        m('span', 'Control'),
        m('span', 'Load Time'),
        registeredPlugins.map((plugin) => this.renderPluginRow(plugin)),
      ),
    );
  }

  private renderPluginRow(plugin: PluginWrapper): m.Children {
    const pluginId = plugin.desc.id;
    const isDefault = defaultPlugins.includes(pluginId);
    const isActive = plugin.active;
    const isEnabled = plugin.enableFlag.get();
    const loadTime = plugin.traceContext?.loadTimeMs;
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
        onclick: () => {
          if (isEnabled) {
            plugin.enableFlag.set(false);
          } else {
            plugin.enableFlag.set(true);
          }
          needsRestart = true;
          raf.scheduleFullRedraw();
        },
      }),
      exists(loadTime)
        ? m('span', `${loadTime.toFixed(1)} ms`)
        : m('span', `-`),
    ];
  }
}
