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
import {CardStack} from '../../widgets/card';
import {Chip} from '../../widgets/chip';
import {Intent} from '../../widgets/common';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {SettingsCard, SettingsShell} from '../../widgets/settings_shell';
import {Switch} from '../../widgets/switch';
import {FuzzyFinder} from '../../base/fuzzy';
import {Stack, StackAuto} from '../../widgets/stack';
import {TextInput} from '../../widgets/text_input';
import {EmptyState} from '../../widgets/empty_state';
import {Popup} from '../../widgets/popup';
import {Box} from '../../widgets/box';
import {Icons} from '../../base/semantic_icons';

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

export interface PluginsPageAttrs {
  readonly subpage?: string;
}

export class PluginsPage implements m.ClassComponent<PluginsPageAttrs> {
  private filterText: string = '';

  view({attrs}: m.Vnode<PluginsPageAttrs>): m.Children {
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
    const subpage = decodeURIComponent(attrs.subpage ?? '');

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
            m(
              Popup,
              {
                trigger: m(Button, {
                  icon: 'restore',
                  disabled: !anyNonDefaults,
                  label: 'Restore Defaults',
                  title: anyNonDefaults
                    ? 'Restore all plugins to their default enabled/disabled state'
                    : 'All plugins are in their default state',
                }),
              },
              m(
                Box,
                m(
                  Stack,
                  'Are you sure you want to restore all plugins to their default enabled/disabled state? This action cannot be undone!',
                  m(
                    Stack,
                    {orientation: 'horizontal'},
                    m(StackAuto),
                    m(Button, {
                      className: Popup.DISMISS_POPUP_GROUP_CLASS,
                      variant: ButtonVariant.Filled,
                      label: 'Cancel',
                    }),
                    m(Button, {
                      className: Popup.DISMISS_POPUP_GROUP_CLASS,
                      intent: Intent.Danger,
                      variant: ButtonVariant.Filled,
                      label: 'Restore Defaults',
                      onclick: () => {
                        for (const plugin of registeredPlugins) {
                          plugin.enableFlag.reset();
                        }
                      },
                    }),
                  ),
                ),
              ),
            ),
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
              filteredPlugins.map(({item: plugin}) => {
                return this.renderPluginCard(
                  plugin,
                  subpage === `/${plugin.desc.id}`,
                );
              }),
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
          title: 'No plugins match your search criteria',
        },
        m(Button, {
          label: 'Clear filter',
          icon: Icons.FilterOff,
          variant: ButtonVariant.Filled,
          intent: Intent.Primary,
          onclick: () => {
            this.filterText = '';
          },
        }),
      );
    } else {
      return m(EmptyState, {
        icon: Icons.NoData,
        title: 'No plugins found',
      });
    }
  }

  private renderPluginCard(
    plugin: PluginWrapper,
    focused: boolean,
  ): m.Children {
    const loadTime = plugin.traceContext?.loadTimeMs;
    return m(SettingsCard, {
      className: classNames(
        'pf-plugins-page__card',
        plugin.enableFlag.get() && 'pf-plugins-page__card--enabled',
      ),
      title: plugin.desc.id,
      linkHref: `#!/plugins/${encodeURIComponent(plugin.desc.id)}`,
      description: plugin.desc.description?.trim(),

      focused: focused,
      controls: m(
        'span.pf-plugins-page__controls',
        exists(loadTime) &&
          m(Chip, {
            className: 'pf-plugins-page__chip',
            label: `STARTUP ${loadTime.toFixed(1)} ms`,
          }),
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
      accent: plugin.enabled
        ? Intent.Success
        : plugin.active
          ? Intent.Warning
          : plugin.enableFlag.get()
            ? Intent.Primary
            : undefined,
    });
  }

  oncreate(vnode: m.VnodeDOM<PluginsPageAttrs>) {
    const subpage = decodeURIComponent(vnode.attrs.subpage ?? '');
    console.log(subpage);
    const pluginId = /[/](.+)/.exec(subpage)?.[1];
    console.log('Scrolling to plugin', pluginId);
    if (pluginId) {
      const plugin = vnode.dom.querySelector(`#${CSS.escape(pluginId)}`);
      console.log('Scrolling to plugin', pluginId, plugin);
      if (plugin) {
        plugin.scrollIntoView({block: 'center'});
      }
    }
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
