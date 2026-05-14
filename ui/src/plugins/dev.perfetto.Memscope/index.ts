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
import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import RecordPageV2 from '../dev.perfetto.RecordTraceV2';
import {ConnectionPage} from './views/connection';
import {Dashboard} from './views/dashboard';
import {LiveSession} from './sessions/live_session';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Memscope';
  static readonly description =
    'Live memory profiler for Android/Linux devices';
  static readonly dependencies = [RecordPageV2];

  static onActivate(app: App) {
    let session: LiveSession | undefined;

    app.sidebar.addMenuItem({
      section: 'trace_files',
      text: 'Memscope',
      href: '#!/memscope',
      icon: 'memory',
      sortOrder: 2.5,
    });

    app.pages.registerPage({
      route: '/memscope',
      render: () => {
        if (session) {
          return m(Dashboard, {
            app,
            session,
            onStopped: () => {
              session?.dispose();
              session = undefined;
            },
          });
        } else {
          return m(ConnectionPage, {
            onConnected: (result) => {
              session = new LiveSession(result);
              session.onSnapshot(() => m.redraw());
            },
          });
        }
      },
    });
  }
}
