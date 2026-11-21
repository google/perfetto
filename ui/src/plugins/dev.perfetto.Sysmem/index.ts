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
import type {Trace} from '../../public/trace';
import MemscopePlugin from '../dev.perfetto.Memscope';
import {ConnectionPage, LiveSession} from '../dev.perfetto.Memscope/live';
import {Dashboard} from './dashboard';
import {
  SYSMEM_TRACE_ROUTE,
  TraceDashboardPage,
  traceHasSysmemData,
} from './trace_dashboard';

export default class SysmemPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Sysmem';
  static readonly description =
    'System-wide memory dashboard for Android/Linux devices and traces';
  static readonly dependencies = [MemscopePlugin];

  static onActivate(app: App) {
    let session: LiveSession | undefined;

    app.sidebar.addMenuItem({
      section: 'trace_files',
      text: 'Sysmem',
      href: '#!/sysmem',
      icon: 'monitoring',
      sortOrder: 2.6,
      badge: 'preview',
    });

    app.pages.registerPage({
      route: '/sysmem',
      render: () => {
        if (session) {
          return m(Dashboard, {
            live: {
              app,
              session,
              onStopped: () => {
                session?.dispose();
                session = undefined;
              },
            },
          });
        }
        return m(ConnectionPage, {
          app,
          title: 'Sysmem',
          description:
            'Connect to an Android device or Linux host to monitor ' +
            'system-wide memory usage in real time via traced.',
          onConnected: (result) => {
            session = new LiveSession(app, result);
            session.onSnapshot(() => m.redraw());
          },
        });
      },
    });
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    trace.pages.registerPage({
      route: SYSMEM_TRACE_ROUTE,
      render: () => m(TraceDashboardPage, {trace}),
    });
    if (!(await traceHasSysmemData(trace.engine))) return;
    trace.sidebar.addMenuItem({
      section: 'current_trace',
      sortOrder: 26,
      text: 'Sysmem',
      href: `#!${SYSMEM_TRACE_ROUTE}`,
      icon: 'monitoring',
      badge: 'preview',
    });
    // Only the Sysmem product opens here; elsewhere this is one view of many.
    if (trace.productId === 'sysmem') {
      trace.initialPage.suggest(SYSMEM_TRACE_ROUTE, 700);
    }
  }
}
