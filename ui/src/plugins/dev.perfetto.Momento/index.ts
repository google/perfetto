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
import {App} from '../../public/app';
import {PerfettoPlugin} from '../../public/plugin';
import {LiveMemoryPage} from './live_memory_page';
import RecordPageV2 from '../dev.perfetto.RecordTraceV2';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Momento';

  // The live memory page depends on record page imports only, not at runtime.
  static readonly dependencies = [RecordPageV2];

  static onActivate(app: App) {
    app.sidebar.addMenuItem({
      section: 'trace_files',
      text: 'Momento',
      href: '#!/momento',
      icon: 'memory',
      sortOrder: 2.5,
    });
    app.pages.registerPage({
      route: '/momento',
      render: () => m(LiveMemoryPage, {app}),
    });
  }
}
