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

import './styles.scss';
import m from 'mithril';
import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import {UxPlaygroundPage} from './ux_playground_page';

// A scratch page for experimenting with UX designs. Add new experiments in
// ux_playground_page.ts. Reachable at #!/ux.
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.UxPlayground';

  static onActivate(app: App): void {
    app.pages.registerPage({
      route: '/ux',
      render: (subpage) => m(UxPlaygroundPage, {app, subpage}),
    });
    app.sidebar.addMenuItem({
      section: 'settings',
      text: 'UX Playground',
      href: '#!/ux',
      icon: 'science',
      sortOrder: 99,
    });
  }
}
