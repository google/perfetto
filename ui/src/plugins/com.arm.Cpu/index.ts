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
import {CpuPage} from './cpu_page';

// Simple plugin that adds an entry in the support section of the sidebar
// to display a page that exposes the loaded Arm telemetry specifications.
export default class implements PerfettoPlugin {
  static readonly id = 'com.arm.CpuPage';

  static onActivate(app: App) {
    app.pages.registerPage({
      route: '/cpu',
      render: () => m(CpuPage, {app}),
    });
    app.sidebar.addMenuItem({
      section: 'support',
      text: 'Cpu',
      href: '#!/cpu',
      icon: 'memory',
    });
  }
}
