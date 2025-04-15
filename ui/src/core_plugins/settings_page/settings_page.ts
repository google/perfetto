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
import {SettingsManagerImpl} from '../../core/settings_manager';
import m from 'mithril';
import {AppImpl} from '../../core/app_impl';
import {z} from 'zod';
import {Button, ButtonBar, ButtonVariant} from '../../widgets/button';
import {Card, CardList} from '../../widgets/card';
import {SettingsShell as SettingsPageWidget} from '../../widgets/settings_shell';
import {Switch} from '../../widgets/switch';
import {Select} from '../../widgets/select';
import {TextInput} from '../../widgets/text_input';
import {Icon} from '../../widgets/icon';
import {Intent} from '../../widgets/common';
import {EmptyState} from '../../widgets/empty_state';
import {classNames} from '../../base/classnames';

export class SettingsPage implements m.ClassComponent {
  private filterText = '';

  view() {
    const app = AppImpl.instance;
    const settingsManager = app.settings as SettingsManagerImpl;
    const allSettings = settingsManager.getAllSettings();
    const reloadRequired = settingsManager.isReloadRequired();

    // Filter settings based on the search text
    const isFiltering = this.filterText.trim() !== '';
    const filteredSettings = isFiltering
      ? allSettings.filter(
          (setting) =>
            setting.name
              .toLowerCase()
              .includes(this.filterText.toLowerCase()) ||
            (setting.description &&
              setting.description
                .toLowerCase()
                .includes(this.filterText.toLowerCase())),
        )
      : allSettings;
    return m(
      SettingsPageWidget,
      {
        title: 'Settings',
        stickyHeaderContent: m(
          '.pf-settings-page__topbar',
          m(
            ButtonBar,
            m(Button, {
              icon: 'restore',
              label: 'Restore Defaults',
              onclick: () => settingsManager.resetAll(),
            }),
            reloadRequired &&
              m(Button, {
                icon: 'refresh',
                label: 'Reload required',
                variant: ButtonVariant.Filled,
                intent: Intent.Primary,
                onclick: () => window.location.reload(),
              }),
          ),
          m(TextInput, {
            placeholder: 'Filter settings...',
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
        filteredSettings.length === 0
          ? this.renderEmptyState(isFiltering)
          : m(
              CardList,
              filteredSettings.map((setting) => {
                return this.renderSettingCard(setting);
              }),
            ),
      ),
    );
  }

  private renderEmptyState(isFiltering: boolean) {
    if (isFiltering) {
      return m(
        EmptyState,
        {
          icon: 'filter_alt_off',
          title: 'No settings match your search criteria',
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
        title: 'No settings found',
      });
    }
  }

  private renderSettingCard(setting: Setting<unknown>) {
    return m(
      Card,
      {
        borderless: true,
        className: classNames(
          'pf-settings-page__card',
          !setting.isDefault && 'pf-settings-page__card--changed',
        ),
        key: setting.id,
      },
      m(
        '.pf-settings-page__details',
        m('h1', setting.name),
        setting.description &&
          m('.pf-settings-page__description', setting.description),
      ),
      m('.pf-settings-page__controls', [
        !setting.isDefault &&
          m(Button, {
            icon: 'restore',
            title: 'Restore default',
            variant: ButtonVariant.Minimal,
            onclick: () => {
              setting.reset();
            },
          }),
        this.renderSettingControl(setting),
      ]),
    );
  }

  private renderSettingControl(setting: Setting<unknown>) {
    const currentValue = setting.get();

    // If the setting has a custom renderer, use it
    if (setting.render) {
      // Cast to any to handle the type mismatch between unknown and T
      return setting.render(setting);
    }

    // Boolean settings get a switch
    if (setting.schema instanceof z.ZodBoolean) {
      return m(Switch, {
        checked: currentValue as boolean,
        onchange: () => {
          setting.set(!(currentValue as boolean));
        },
      });
    }

    // Enum settings get a select dropdown
    else if (setting.schema instanceof z.ZodEnum) {
      const options = setting.schema.options;
      return m(
        Select,
        {
          value: String(currentValue), // Ensure value is a string
          onchange: (e: Event) => {
            const target = e.target as HTMLSelectElement;
            setting.set(target.value);
          },
        },
        options.map((option: string) => {
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
    }

    // Native enum settings also get a select dropdown
    else if (setting.schema instanceof z.ZodNativeEnum) {
      // Extract the enum values - for native enums we need to get both keys and values
      const enumValues = Object.entries(setting.schema._def.values);

      return m(
        Select,
        {
          value: String(currentValue), // Ensure value is a string
          onchange: (e: Event) => {
            const target = e.target as HTMLSelectElement;
            // Convert to number if the original enum value was numeric
            const value = isNaN(Number(target.value))
              ? target.value
              : Number(target.value);
            setting.set(value);
          },
        },
        enumValues.map(([key, value]) => {
          // Only include the string keys (not the reverse mapping that TypeScript adds)
          if (typeof key === 'string' && isNaN(Number(key))) {
            return m(
              'option',
              {
                value: value,
                selected: currentValue === value,
              },
              key, // Display the enum key (name) to the user
            );
          }
          return null;
        }),
      );
    }

    // Number settings get a number input
    else if (setting.schema instanceof z.ZodNumber) {
      const minCheck = setting.schema._def.checks.find(
        (check) => check.kind === 'min',
      );
      const maxCheck = setting.schema._def.checks.find(
        (check) => check.kind === 'max',
      );
      const min = minCheck ? minCheck.value : undefined;
      const max = maxCheck ? maxCheck.value : undefined;

      return m(TextInput, {
        type: 'number',
        value: currentValue as number,
        min: min, // Add min attribute
        max: max, // Add max attribute
        onchange: (e: Event) => {
          const target = e.target as HTMLInputElement;
          const value = target.valueAsNumber;
          setting.set(value);
        },
      });
    }

    // String settings get a text input
    else if (setting.schema instanceof z.ZodString) {
      return m(TextInput, {
        value: currentValue as string,
        onchange: (e: Event) => {
          const target = e.target as HTMLInputElement;
          setting.set(target.value);
        },
      });
    }

    // For complex types or unsupported schemas, just show an error message
    else {
      return m('.pf-settings-page__complex-error', [
        m(Icon, {icon: 'error_outline'}),
        m('span', 'Cannot edit this setting directly'),
      ]);
    }
  }
}
