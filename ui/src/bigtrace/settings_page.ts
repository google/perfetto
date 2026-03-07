// Copyright (C) 2026 The Android Open Source Project
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
import {SettingsShell, SettingsCard} from '../widgets/settings_shell';
import {settingsManager} from './settings_manager';
import {Select} from '../widgets/select';
import {Setting} from '../public/settings';

export class SettingsPage implements m.ClassComponent {
  view() {
    return m(
        SettingsShell,
        {
            title: 'Settings',
            className: 'page',
        },
        m(
            '.pf-settings-page',
            settingsManager.getAllSettings().map((setting) => {
                return this.renderSettingCard(setting);
            }),
        ),
    );
  }

  private renderSettingCard(setting: Setting<unknown>) {
    return m(SettingsCard, {
      id: setting.id,
      title: setting.name,
      description: setting.description,
      controls: this.renderSettingControl(setting),
    });
  }

  private renderSettingControl(setting: Setting<unknown>) {
    const currentValue = setting.get();
    return m(
        Select,
        {
          value: String(currentValue),
          onchange: (e: Event) => {
            const target = e.target as HTMLSelectElement;
            setting.set(target.value);
          },
        },
        ['light', 'dark'].map((option) => {
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
}
