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

import {AppImpl} from '../../core/app_impl';
import {PerfettoPlugin} from '../../public/plugin';
import {FlagsPage} from './flags_page';
import {PluginsPage} from './plugins_page';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.FlagsPage';

  static onActivate(app: AppImpl) {
    // Flags page
    app.pages.registerPage({
      route: '/flags',
      page: FlagsPage,
      traceless: true,
    });
    app.sidebar.addMenuItem({
      section: 'support',
      sortOrder: 3,
      text: 'Flags',
      href: '#!/flags',
      icon: 'emoji_flags',
    });

    // Plugins page.
    app.pages.registerPage({
      route: '/plugins',
      page: PluginsPage,
      traceless: true,
    });
    app.sidebar.addMenuItem({
      section: 'support',
      text: 'Plugins',
      href: '#!/plugins',
      icon: 'extension',
      sortOrder: 9,
    });
  }
}
