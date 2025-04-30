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

import m from 'mithril';
import {z} from 'zod';
import {App} from '../../public/app';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {Button} from '../../widgets/button';

export default class implements PerfettoPlugin {
  static readonly id = 'com.example.Settings';
  static readonly description =
    'Example plugin to show how to register settings.';

  static onActivate(app: App) {
    // Register a simple boolean setting like this. The setting will appear on
    // the settings page as a toggle switch.
    const booleanSetting = app.settings.register({
      id: 'com.example.Settings#booleanSetting',
      name: 'Boolean Setting',
      description: 'A boolean setting.',
      schema: z.boolean(),
      defaultValue: false,
    });

    // Read the setting like this
    console.log(`The value of the boolean setting is: ${booleanSetting.get()}`);

    // Write it like this (e.g. from a command)
    app.commands.registerCommand({
      id: 'com.example.Settings#toggleBooleanSetting',
      name: 'Toggle Boolean Setting',
      callback: () => {
        // Toggle the boolean setting
        booleanSetting.set(!booleanSetting.get());
        console.log(`New value: ${booleanSetting.get()}`);
      },
    });

    // This is how you register a string setting. The setting will appear on
    // the settings page as a nubmer input.
    app.settings.register({
      id: 'com.example.Settings#numberSetting',
      name: 'Number Setting',
      description: 'A numerical setting.',
      schema: z.number().min(0).max(100),
      defaultValue: 50,
    });

    // Enum values are supplied through the zod schema.
    app.settings.register({
      id: 'com.example.Settings#enumSetting',
      name: 'Enum Setting',
      description: 'An enum setting.',
      schema: z.enum(['high', 'medium', 'low']),
      defaultValue: 'medium',
    });

    // For more complex settings (more complex than e.g. numbers, booleans,
    // strings, etc), you must pass a custom renderer to render the settings
    // configurator on the settings page.
    app.settings.register({
      id: 'com.example.Settings#objectSetting',
      name: 'Object Setting',
      description: 'A complex object requiring a custom renderer.',
      schema: z.object({foo: z.boolean()}),
      defaultValue: {foo: false},
      render: (setting) => {
        return m(
          'div',
          [m('span', JSON.stringify(setting.get()))],
          m(Button, {
            label: 'Toggle nested boolean',
            onclick: () => {
              setting.set({foo: !setting.get().foo});
            },
          }),
        );
      },
    });

    // Set the requiresReload flag to true to indicate that the user should be
    // prompted to reload the app after changing this setting.
    app.settings.register({
      id: 'com.example.Settings#reloadRequired',
      name: 'Boolean Setting - Requires Reload',
      description:
        'After changing this setting, the user will be prompted to reload.',
      schema: z.boolean(),
      defaultValue: false,
      requiresReload: true,
    });
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    // Register a setting that is only available when a trace is loaded.
    trace.settings.register({
      id: 'com.example.Settings#booleanSettingWithTrace',
      name: 'Boolean Setting (registered with the trace)',
      description:
        "A boolean setting that's registered with teh trace rather than the app.",
      schema: z.boolean(),
      defaultValue: false,
    });
  }
}
