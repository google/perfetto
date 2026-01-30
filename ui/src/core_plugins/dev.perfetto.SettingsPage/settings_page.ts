// Copyright (C) 2025 The Android Open Source Project
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

import {Setting} from '../../public/settings';
import {SettingImpl, SettingsManagerImpl} from '../../core/settings_manager';
import m from 'mithril';
import {AppImpl} from '../../core/app_impl';
import {Button, ButtonVariant} from '../../widgets/button';
import {getZodSchemaInfo} from '../../base/zod_utils';
import {CardStack} from '../../widgets/card';
import {SettingsCard, SettingsShell} from '../../widgets/settings_shell';
import {Switch} from '../../widgets/switch';
import {Select} from '../../widgets/select';
import {TextInput} from '../../widgets/text_input';
import {Icon} from '../../widgets/icon';
import {Intent} from '../../widgets/common';
import {EmptyState} from '../../widgets/empty_state';
import {Stack, StackAuto} from '../../widgets/stack';
import {FuzzyFinder, FuzzySegment} from '../../base/fuzzy';
import {Popup} from '../../widgets/popup';
import {Box} from '../../widgets/box';
import {Icons} from '../../base/semantic_icons';
import {GateDetector} from '../../base/mithril_utils';
import {findRef} from '../../base/dom_utils';

const SEARCH_BOX_REF = 'settings-search-box';

export interface SettingsPageAttrs {
  readonly subpage?: string;
}

export class SettingsPage implements m.ClassComponent<SettingsPageAttrs> {
  private filterText = '';

  view({attrs}: m.Vnode<SettingsPageAttrs>): m.Children {
    const app = AppImpl.instance;
    const settingsManager = app.settings as SettingsManagerImpl;
    const reloadRequired = settingsManager.isReloadRequired();
    const isFiltering = this.filterText.trim() !== '';
    const subpage = decodeURIComponent(attrs.subpage ?? '');

    // Get settings (filtered or all) grouped by plugin
    const settings = isFiltering
      ? this.getFilteredSettingsGrouped(settingsManager)
      : this.getAllSettingsGrouped(settingsManager);
    const groupedSettings = this.groupSettingsByPlugin(settings);

    // Sort plugin IDs: CORE_PLUGIN_ID first, then alphabetically
    const sortedPluginIds = Array.from(groupedSettings.keys()).sort((a, b) => {
      if (!a) return -1;
      if (!b) return 1;
      return a.localeCompare(b);
    });

    const page = m(
      SettingsShell,
      {
        title: 'Settings',
        className: 'page',
        stickyHeaderContent: m(
          Stack,
          {orientation: 'horizontal'},
          m(
            Popup,
            {
              trigger: m(Button, {
                icon: 'restore',
                label: 'Restore Defaults',
              }),
            },
            m(
              Box,
              m(
                Stack,
                'Are you sure you want to restore all settings to their default values? This action cannot be undone!',
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
                    onclick: () => settingsManager.resetAll(),
                  }),
                ),
              ),
            ),
          ),
          reloadRequired &&
            m(Button, {
              icon: 'refresh',
              label: 'Reload required',
              variant: ButtonVariant.Filled,
              intent: Intent.Primary,
              onclick: () => window.location.reload(),
            }),
          m(StackAuto),
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
        '.pf-settings-page',
        groupedSettings.size === 0
          ? this.renderEmptyState(isFiltering)
          : sortedPluginIds.map((pluginId) => {
              const settings = groupedSettings.get(pluginId)!;
              return this.renderPluginSection(pluginId, settings, subpage);
            }),
      ),
    );

    return m(
      GateDetector,
      {
        onVisibilityChanged: (visible: boolean, dom: Element) => {
          if (visible) {
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

  private getAllSettingsGrouped(settingsManager: SettingsManagerImpl) {
    return settingsManager
      .getAllSettings()
      .map((item) => ({item, segments: []}));
  }

  private getFilteredSettingsGrouped(settingsManager: SettingsManagerImpl) {
    const allSettings = settingsManager.getAllSettings();
    const finder = new FuzzyFinder(allSettings, (s) => {
      return `${s.name} ${s.description ?? ''}`;
    });
    return finder.find(this.filterText);
  }

  private groupSettingsByPlugin(
    settings: Array<{item: SettingImpl<unknown>; segments: FuzzySegment[]}>,
  ) {
    const app = AppImpl.instance;
    const grouped = new Map<
      string,
      Array<{item: Setting<unknown>; segments: FuzzySegment[]}>
    >();
    for (const result of settings) {
      const setting = result.item;
      const isCore =
        setting.pluginId === undefined ||
        app.plugins.isCorePlugin(setting.pluginId);
      const targetGroup = isCore ? 'Core' : setting.pluginId;

      const existing = grouped.get(targetGroup) ?? [];
      existing.push(result);
      grouped.set(targetGroup, existing);
    }
    return grouped;
  }

  private renderPluginSection(
    pluginId: string,
    settings: Array<{item: Setting<unknown>; segments: FuzzySegment[]}>,
    subpage: string,
  ) {
    return m(
      '.pf-settings-page__plugin-section',
      {key: pluginId},
      m('h2.pf-settings-page__plugin-title', pluginId),
      m(
        CardStack,
        settings.map(({item}) => {
          return this.renderSettingCard(item, subpage);
        }),
      ),
    );
  }

  private renderEmptyState(isFiltering: boolean) {
    if (isFiltering) {
      return m(
        EmptyState,
        {
          title: 'No settings match your search criteria',
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
        icon: 'search_off',
        title: 'No settings found',
      });
    }
  }

  private renderSettingCard(setting: Setting<unknown>, subpage: string) {
    return m(SettingsCard, {
      id: setting.id,
      title: setting.name,
      description: setting.description.trim(),
      focused: subpage === `/${setting.id}`,
      controls: m('.pf-settings-page__controls', [
        !setting.isDefault &&
          m(Button, {
            icon: 'restore',
            title: 'Restore default',
            variant: ButtonVariant.Minimal,
            className: 'pf-settings-page__restore-button',
            onclick: () => {
              setting.reset();
            },
          }),
        this.renderSettingControl(setting),
      ]),
      accent: !setting.isDefault ? Intent.Primary : undefined,
      linkHref: `#!/settings/${encodeURIComponent(setting.id)}`,
    });
  }

  private renderSettingControl(setting: Setting<unknown>) {
    const currentValue = setting.get();

    // If the setting has a custom renderer, use it
    if (setting.render) {
      // Cast to any to handle the type mismatch between unknown and T
      return setting.render(setting);
    }

    const schemaInfo = getZodSchemaInfo(setting.schema);

    switch (schemaInfo.kind) {
      case 'boolean':
        return m(Switch, {
          checked: currentValue as boolean,
          onchange: () => {
            setting.set(!(currentValue as boolean));
          },
        });

      case 'enum':
        return m(
          Select,
          {
            value: String(currentValue),
            onchange: (e: Event) => {
              const target = e.target as HTMLSelectElement;
              setting.set(target.value);
            },
          },
          schemaInfo.options.map((option) => {
            return m(
              'option',
              {
                value: option,
                selected: currentValue === option,
              },
              option,
            );
          }),
        );

      case 'number':
        return m(TextInput, {
          type: 'number',
          value: currentValue as number,
          min: schemaInfo.min,
          max: schemaInfo.max,
          onchange: (e: Event) => {
            const target = e.target as HTMLInputElement;
            const value = target.valueAsNumber;
            setting.set(value);
          },
        });

      case 'string':
        return m(TextInput, {
          value: currentValue as string,
          onchange: (e: Event) => {
            const target = e.target as HTMLInputElement;
            setting.set(target.value);
          },
        });

      case 'unknown':
      default:
        return m('.pf-settings-page__complex-error', [
          m(Icon, {icon: 'error_outline'}),
          m('span', 'Cannot edit this setting directly'),
        ]);
    }
  }
}
