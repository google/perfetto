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
import {App} from '../../public/app';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {SettingsPage} from './settings_page';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.SettingsPage';

  static onActivate(app: App) {
    app.sidebar.addMenuItem({
      section: 'settings',
      sortOrder: 1,
      text: 'Settings',
      href: '#!/settings',
      icon: 'settings',
    });

    app.pages.registerPage({
      route: '/settings',
      render: (subpage) => m(SettingsPage, {subpage}),
    });

    app.commands.registerCommand({
      id: 'dev.perfetto.OpenSettings',
      name: 'Open Settings',
      callback: () => {
        app.navigate('#!/settings');
      },
    });
  }

  async onTraceLoad(_: Trace) {
    // Nothing to do here.
  }
}
