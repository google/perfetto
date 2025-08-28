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
import {classNames} from '../../base/classnames';
import {assertUnreachable} from '../../base/logging';
import {exists} from '../../base/utils';
import {AppImpl} from '../../core/app_impl';
import {PluginWrapper} from '../../core/plugin_manager';
import {Button, ButtonBar, ButtonVariant} from '../../widgets/button';
import {Card, CardStack} from '../../widgets/card';
import {Chip} from '../../widgets/chip';
import {Intent} from '../../widgets/common';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {SettingsShell} from '../../widgets/settings_shell';
import {Switch} from '../../widgets/switch';
import {FuzzyFinder} from '../../base/fuzzy';
import {Stack, StackAuto} from '../../widgets/stack';
import {TextInput} from '../../widgets/text_input';
import {EmptyState} from '../../widgets/empty_state';

enum SortOrder {
  Name = 'name',
  Slowest = 'slowest',
  Enabled = 'enabled',
  Disabled = 'disabled',
}

let sortOrder = SortOrder.Name;

function sortPlugins(registeredPlugins: ReadonlyArray<PluginWrapper>) {
  switch (sortOrder) {
    case SortOrder.Slowest:
      return registeredPlugins.concat([]).sort((a, b) => {
        return (
          (b.traceContext?.loadTimeMs ?? -1) -
          (a.traceContext?.loadTimeMs ?? -1)
        );
      });
    case SortOrder.Name:
      return registeredPlugins.concat([]).sort((a, b) => {
        return a.desc.id.localeCompare(b.desc.id);
      });
    case SortOrder.Enabled:
      return registeredPlugins.concat([]).sort((a, b) => {
        return (b.enabled ? 1 : 0) - (a.enabled ? 1 : 0);
      });
    case SortOrder.Disabled:
      return registeredPlugins.concat([]).sort((a, b) => {
        return (a.enabled ? 1 : 0) - (b.enabled ? 1 : 0);
      });
    default:
      assertUnreachable(sortOrder);
  }
}

function sortText(sortOrder: SortOrder) {
  switch (sortOrder) {
    case SortOrder.Slowest:
      return 'Startup time (slowest first)';
    case SortOrder.Name:
      return 'Name';
    case SortOrder.Enabled:
      return 'Enabled first';
    case SortOrder.Disabled:
      return 'Disabled first';
    default:
      assertUnreachable(sortOrder);
  }
}

export class PluginsPage implements m.ClassComponent {
  private filterText: string = '';

  view() {
    const pluginManager = AppImpl.instance.plugins;
    const registeredPlugins = pluginManager.getAllPlugins();
    const needsRestart = registeredPlugins.some((p) => {
      return p.enableFlag.get() !== p.enabled;
    });
    const anyNonDefaults = registeredPlugins.some((p) => {
      return p.enableFlag.isOverridden();
    });
    const sorted = sortPlugins(registeredPlugins);

    const isFiltering = this.filterText !== '';
    const finder = new FuzzyFinder(sorted, (p) => {
      return `${p.desc.id} ${p.desc.description ?? ''}`;
    });
    const filteredPlugins = isFiltering
      ? finder.find(this.filterText)
      : sorted.map((item) => ({item, segments: []}));

    return m(
      SettingsShell,
      {
        title: 'Plugins',
        stickyHeaderContent: m(
          Stack,
          {
            className: 'pf-plugins-page__topbar',
            orientation: 'horizontal',
          },
          m(
            ButtonBar,
            m(Button, {
              icon: 'restore',
              disabled: !anyNonDefaults,
              label: 'Restore Defaults',
              title: anyNonDefaults
                ? 'Restore all plugins to their default enabled/disabled state'
                : 'All plugins are in their default state',
              onclick: () => {
                for (const plugin of registeredPlugins) {
                  plugin.enableFlag.reset();
                }
              },
            }),
            needsRestart && reloadButton(),
          ),
          m(StackAuto),
          m(
            PopupMenu,
            {
              trigger: m(Button, {
                icon: 'sort',
                label: `Sort by ${sortText(sortOrder)}`,
              }),
            },
            Object.values(SortOrder).map((value) => {
              return m(MenuItem, {
                label: sortText(value),
                active: sortOrder === value,
                onclick: () => (sortOrder = value),
              });
            }),
          ),
          m(TextInput, {
            placeholder: 'Search...',
            value: this.filterText,
            leftIcon: 'search',
            oninput: (e: Event) => {
              const target = e.target as HTMLInputElement;
              this.filterText = target.value;
            },
          }),
        ),
      },
      m(
        '.pf-plugins-page',
        filteredPlugins.length > 0
          ? m(
              CardStack,
              filteredPlugins.map(({item}) => this.renderPluginCard(item)),
            )
          : this.renderEmptyState(isFiltering),
      ),
    );
  }

  private renderEmptyState(isFiltering: boolean) {
    if (isFiltering) {
      return m(
        EmptyState,
        {
          icon: 'filter_alt_off',
          title: 'No plugins match your search criteria',
        },
        m(Button, {
          label: 'Clear filter',
          icon: 'clear',
          variant: ButtonVariant.Filled,
          intent: Intent.Primary,
          onclick: () => {
            this.filterText = '';
          },
        }),
      );
    } else {
      return m(EmptyState, {
        icon: 'search_off',
        title: 'No plugins found',
      });
    }
  }

  private renderPluginCard(plugin: PluginWrapper): m.Children {
    const loadTime = plugin.traceContext?.loadTimeMs;
    return m(
      Card,
      {
        className: classNames(
          'pf-plugins-page__card',
          plugin.active && 'pf-plugins-page__card--active',
          plugin.enableFlag.get() && 'pf-plugins-page__card--enabled',
        ),
        key: plugin.desc.id,
      },
      m(
        '.pf-plugins-page__details',
        m('h1', plugin.desc.id),
        plugin.desc.description &&
          m('.pf-plugins-page__description', plugin.desc.description),
      ),
      m(
        '.pf-plugins-page__controls',
        // plugin.enabled !== plugin.enableFlag.get() && reloadButton(),
        exists(loadTime) &&
          m(
            'span',
            m(Chip, {
              className: 'pf-plugins-page__chip',
              label: `STARTUP ${loadTime.toFixed(1)} ms`,
            }),
          ),
        m(Switch, {
          checked: plugin.enableFlag.get(),
          onchange: () => {
            if (plugin.enableFlag.isOverridden()) {
              plugin.enableFlag.reset();
            } else {
              plugin.enableFlag.set(!plugin.enableFlag.get());
            }
          },
        }),
      ),
    );
  }
}

function reloadButton() {
  return m(Button, {
    icon: 'refresh',
    label: 'Reload required',
    intent: Intent.Primary,
    variant: ButtonVariant.Filled,
    title: 'Click here to reload the page',
    onclick: () => location.reload(),
  });
}
