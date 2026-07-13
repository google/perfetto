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
import type {PerfettoPlugin, PluginStatus} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import RecordPageV2 from '../dev.perfetto.RecordTraceV2';
import {ConnectionPage} from './views/connection';
import {Dashboard} from './views/dashboard';
import {LiveSession} from './sessions/live_session';
import {MemoryOverviewPage} from './views/landing_page/landing_page';
import {NUM} from '../../trace_processor/query_result';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Memscope';
  static readonly description =
    'Live memory profiler for Android/Linux devices';
  static readonly status: PluginStatus = 'experimental';
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
              session = new LiveSession(app, result);
              session.onSnapshot(() => m.redraw());
            },
          });
        }
      },
    });
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    const pageRoot = '/memoryoverview';

    trace.pages.registerPage({
      route: pageRoot,
      render: (subpage) =>
        m(MemoryOverviewPage, {
          trace,
          subpage,
          onSubpageChange: (subpage) => {
            trace.navigate(`#!${pageRoot}/${subpage}`);
          },
        }),
    });

    trace.sidebar.addMenuItem({
      section: 'current_trace',
      sortOrder: 25,
      text: 'Memory Overview',
      href: `#!${pageRoot}`,
      icon: 'memory',
    });

    // Only suggest the page as the initial view when the trace actually has
    // some memory data — java heap dumps, smaps snapshots, or native
    // (heapprofd) profiles. On a trace with none of these the page has nothing
    // to show.
    if (await this.hasMemoryInfo(trace)) {
      // Make this page appear before the heap dump explorer page.
      trace.initialPage.suggest(pageRoot, 500);
    }
  }

  private async hasMemoryInfo(trace: Trace): Promise<boolean> {
    const counts = await trace.engine.query(`
      SELECT
        (SELECT count(DISTINCT graph_sample_ts) FROM heap_graph_object)
          AS heapDumps,
        (SELECT count(DISTINCT ts) FROM profiler_smaps) AS smapsSnapshots,
        (SELECT count(DISTINCT ts) FROM heap_profile_allocation) AS nativeDumps
    `);
    const {heapDumps, smapsSnapshots, nativeDumps} = counts.firstRow({
      heapDumps: NUM,
      smapsSnapshots: NUM,
      nativeDumps: NUM,
    });

    return heapDumps + smapsSnapshots + nativeDumps >= 1;
  }
}
