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
import {bigTraceSettingsManager} from './bigtrace_settings_manager';
import {Setting} from '../public/settings';
import {z} from 'zod';

export class BigTraceSettingsPage implements m.ClassComponent {
  view() {
    return m(
        SettingsShell,
        {
            title: 'BigTrace Settings',
            className: 'page',
        },
        m(
            '.pf-settings-page',
            bigTraceSettingsManager.getAllSettings().map((setting) => {
                if (setting.id === 'traceLimit') {
                    return this.renderSettingCard(setting);
                }
                return undefined;
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
    if (setting.schema instanceof z.ZodNumber) {
        return m('input.pf-text-input[type=number]', {
            value: currentValue,
            onchange: (e: Event) => {
                const newLimit = parseInt(
                    (e.target as HTMLInputElement).value,
                    10,
                );
                if (!isNaN(newLimit) && newLimit > 0) {
                    setting.set(newLimit);
                }
            },
        });
    }
    return undefined;
  }
}
