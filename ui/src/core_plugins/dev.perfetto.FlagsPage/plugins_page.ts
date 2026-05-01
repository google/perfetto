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
import {assertUnreachable} from '../../base/assert';
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
import {Anchor} from '../../widgets/anchor';
import {Icon} from '../../widgets/icon';
import {Icons} from '../../base/semantic_icons';
import {GateDetector} from '../../base/mithril_utils';
import {findRef} from '../../base/dom_utils';

const SEARCH_BOX_REF = 'plugin-search-box';

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
  private readonly dependenciesByPluginId: ReadonlyMap<
    string,
    ReadonlyArray<PluginWrapper>
  >;
  private readonly dependantsByPluginId: ReadonlyMap<
    string,
    ReadonlyArray<PluginWrapper>
  >;

  constructor() {
    const allPlugins = AppImpl.instance.plugins.getAllPlugins();
    const byId = new Map(allPlugins.map((p) => [p.desc.id, p]));
    const dependencies = new Map<string, PluginWrapper[]>();
    const dependants = new Map<string, PluginWrapper[]>();
    for (const p of allPlugins) {
      dependencies.set(p.desc.id, []);
      dependants.set(p.desc.id, []);
    }
    for (const p of allPlugins) {
      for (const dep of p.desc.dependencies ?? []) {
        const target = byId.get(dep.id);
        if (!target) continue;
        dependencies.get(p.desc.id)!.push(target);
        dependants.get(target.desc.id)!.push(p);
      }
    }
    this.dependenciesByPluginId = dependencies;
    this.dependantsByPluginId = dependants;
  }

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

    const page = m(
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
            ref: SEARCH_BOX_REF,
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

    return m(
      GateDetector,
      {
        onVisibilityChanged: (visible: boolean, dom: Element) => {
          if (visible) {
            // Focus the search input
            const input = findRef(
              dom,
              SEARCH_BOX_REF,
            ) as HTMLInputElement | null;
            if (input) {
              input.focus();
              input.select();
            }
          }
        },
      },
      page,
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
    const dependencyPlugins =
      this.dependenciesByPluginId.get(plugin.desc.id) ?? [];
    const dependantPlugins =
      this.dependantsByPluginId.get(plugin.desc.id) ?? [];
    return m(SettingsCard, {
      id: plugin.desc.id,
      className: classNames(
        'pf-plugins-page__card',
        plugin.enableFlag.get() && 'pf-plugins-page__card--enabled',
      ),
      title: plugin.desc.id,
      linkHref: `#!/plugins/${encodeURIComponent(plugin.desc.id)}`,
      description: [
        plugin.desc.description?.trim(),
        renderPluginIdList(
          'Requires',
          'account_tree',
          'pf-plugins-page__deps--dependencies',
          dependencyPlugins,
        ),
        renderPluginIdList(
          'Required by',
          'hub',
          'pf-plugins-page__deps--dependants',
          dependantPlugins,
        ),
      ],

      focused: focused,
      controls: m(
        'span.pf-plugins-page__controls',
        plugin.active &&
          !plugin.enabled &&
          m(Chip, {
            className: 'pf-plugins-page__chip',
            label: 'TRANSITIVELY ENABLED',
            intent: Intent.Warning,
            title:
              'This plugin is disabled but has been activated because another active plugin requires it. See the Required by list below.',
          }),
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
}

function renderPluginIdList(
  label: string,
  icon: string,
  variantClass: string,
  plugins: ReadonlyArray<PluginWrapper>,
) {
  if (plugins.length === 0) return null;
  return m(
    `.pf-plugins-page__deps.${variantClass}`,
    m(Icon, {icon, className: 'pf-plugins-page__deps-icon'}),
    m('span.pf-plugins-page__deps-label', `${label}: `),
    plugins.map((p) => {
      const id = p.desc.id;
      const isActive = Boolean(p.active);
      return m(
        Anchor,
        {
          href: `#!/plugins/${encodeURIComponent(id)}`,
          className: 'pf-plugins-page__deps-link',
          title: isActive ? `${id} (active)` : `${id} (inactive)`,
        },
        m(Chip, {
          className: 'pf-plugins-page__chip',
          label: id,
          icon: isActive ? 'check_circle' : 'radio_button_unchecked',
          intent: isActive ? Intent.Success : undefined,
        }),
      );
    }),
  );
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
