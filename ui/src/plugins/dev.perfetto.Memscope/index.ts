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
import {z} from 'zod';
import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Setting} from '../../public/settings';
import type {Trace} from '../../public/trace';
import RecordPageV2 from '../dev.perfetto.RecordTraceV2';
import HeapDumpExplorerPlugin from '../com.android.HeapDumpExplorer';
import {ConnectionPage} from './views/connection';
import {Dashboard} from './views/dashboard';
import {LiveSession} from './sessions/live_session';
import {MemoryExplorerPage} from './memory_explorer/page';
import {MEMSCOPE_SQL_PACKAGE} from './product_assets';
import {
  MEMORY_EXPLORER_ROUTE,
  memoryExplorerHref,
} from './memory_explorer/route';
import {NUM} from '../../trace_processor/query_result';
import {EmptyState} from '../../widgets/empty_state';

export {memoryExplorerHref, memoryExplorerRoute} from './memory_explorer/route';

export default class MemscopePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Memscope';
  static readonly description =
    'Live memory profiler for Android/Linux devices';
  static readonly dependencies = [RecordPageV2, HeapDumpExplorerPlugin];
  private static openByDefaultSetting: Setting<boolean>;
  private static hideDefaultChangedHintSetting: Setting<boolean>;

  static onActivate(app: App) {
    HeapDumpExplorerPlugin.hostedElsewhere = true;
    app.addSqlPackages([MEMSCOPE_SQL_PACKAGE]);
    MemscopePlugin.openByDefaultSetting = app.settings.register({
      id: 'dev.perfetto.OpenMemoryOverviewByDefault',
      name: 'Open Memory Explorer by default',
      description:
        'Open traces containing heap dumps, smaps snapshots or native heap ' +
        'profiles in Memory Explorer instead of the timeline.',
      schema: z.boolean(),
      defaultValue: true,
    });

    MemscopePlugin.hideDefaultChangedHintSetting = app.settings.register({
      id: 'dev.perfetto.HideMemoryOverviewDefaultChangedHint',
      name: 'Hide Memory Explorer default-page explanation',
      description:
        'Do not show the explanation that Memory Explorer is the default ' +
        'page for traces containing memory data.',
      schema: z.boolean(),
      defaultValue: false,
      headless: true,
    });

    let session: LiveSession | undefined;

    app.sidebar.addMenuItem({
      section: 'trace_files',
      text: 'Memscope',
      href: '#!/memscope',
      icon: 'memory',
      sortOrder: 2.5,
      badge: 'preview',
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
            app,
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
    const openByDefault = MemscopePlugin.openByDefaultSetting;
    const hideDefaultChangedHint = MemscopePlugin.hideDefaultChangedHintSetting;
    if (!(await traceHasMemoryData(trace))) return;

    const session = trace.plugins.getPlugin(HeapDumpExplorerPlugin).session;
    session.setRouteBuilder(
      (upid, sub) =>
        `#!${MEMORY_EXPLORER_ROUTE}` +
        (upid === undefined ? '' : `/${upid}/${sub ?? 'summary'}`),
    );

    // A permalink restored heap dump state: land on it.
    const restoredSub = session.navSubpage;
    const restoredUpid = session.activeDump?.upid;
    const restored = restoredSub !== undefined && restoredUpid !== undefined;
    const autoNavigated = !restored && openByDefault.get();

    trace.pages.registerPage({
      route: MEMORY_EXPLORER_ROUTE,
      render: (subpage) =>
        m(MemoryExplorerPage, {
          trace,
          session,
          subpage,
          autoNavigated,
          openByDefault,
          hideDefaultChangedHint,
        }),
    });

    // Old Memory Overview links: /memoryoverview/<upid>[/smaps].
    trace.pages.registerPage({
      route: '/memoryoverview',
      render: (subpage) => {
        const [upid, tab] = (subpage ?? '').split('/').filter((x) => x);
        const href =
          upid === undefined
            ? memoryExplorerHref()
            : `${memoryExplorerHref()}/${upid}/${tab === 'smaps' ? 'smaps' : 'summary'}`;
        location.replace(href);
        return m(EmptyState, {icon: 'hourglass', title: 'Redirecting...'});
      },
    });

    trace.sidebar.addMenuItem({
      section: 'current_trace',
      sortOrder: 25,
      text: 'Memory Explorer',
      href: memoryExplorerHref(),
      icon: 'memory',
      badge: 'preview',
    });

    if (restored) {
      trace.initialPage.suggest(
        `${MEMORY_EXPLORER_ROUTE}/${restoredUpid}/${restoredSub}`,
        600,
      );
    } else if (autoNavigated) {
      trace.initialPage.suggest(MEMORY_EXPLORER_ROUTE, 500);
    }
  }
}

// Whether the trace has any data the Memory Explorer can show.
async function traceHasMemoryData(trace: Trace): Promise<boolean> {
  const res = await trace.engine.query(`
    SELECT
      EXISTS(SELECT 1 FROM heap_graph)
      OR EXISTS(SELECT 1 FROM profiler_smaps)
      OR EXISTS(SELECT 1 FROM heap_profile_allocation) AS has
  `);
  return res.firstRow({has: NUM}).has !== 0;
}
