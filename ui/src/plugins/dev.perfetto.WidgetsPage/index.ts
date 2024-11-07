// Copyright (C) 2024 The Android Open Source Project
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

import {App} from '../../public/app';
import {PerfettoPlugin} from '../../public/plugin';
import {WidgetsPage} from './widgets_page';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.WidgetsPage';

  static onActivate(app: App): void {
    app.pages.registerPage({
      route: '/widgets',
      page: WidgetsPage,
      traceless: true,
    });
    app.sidebar.addMenuItem({
      section: 'navigation',
      text: 'Widgets',
      href: '#!/widgets',
      icon: 'widgets',
      sortOrder: 99,
    });
  }
}
