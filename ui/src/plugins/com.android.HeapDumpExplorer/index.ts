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
import {NUM} from '../../trace_processor/query_result';
import HeapProfilePlugin, {
  traceHasTimelineData,
} from '../dev.perfetto.HeapProfile';
import {HeapDumpPage} from './heap_dump_page';
import {HeapDumpExplorerSession} from './session';
import {migrateHdeState} from './persisted_state';
import type {time} from '../../base/time';

const PLUGIN_ID = 'com.android.HeapDumpExplorer';

export default class HeapDumpExplorerPlugin implements PerfettoPlugin {
  static readonly id = PLUGIN_ID;
  static readonly dependencies = [HeapProfilePlugin];
  private static defaultFlamegraphSetting: Setting<boolean>;
  private readonly session: HeapDumpExplorerSession;

  static onActivate(app: App) {
    HeapDumpExplorerPlugin.defaultFlamegraphSetting = app.settings.register({
      id: 'com.android.HeapDumpExplorerDefaultFlamegraph',
      name: 'Heap Dump Explorer: Default to Flamegraph',
      description:
        'Make the flamegraph the first selected tab rather than the overview page in Heap Dump Explorer',
      schema: z.boolean(),
      defaultValue: false,
    });
  }

  constructor(trace: Trace) {
    // The core restores this store (phase 1) before plugins run, so the session
    // reads any shared-link state straight from it.
    const store = trace.mountStore(PLUGIN_ID, migrateHdeState);

    const hideDefaultChangedHint = trace.settings.register({
      id: 'com.android.HideHeapDumpExplorerDefaultChangedHint',
      name: 'Hide Heap Dump Explorer Explanation',
      description:
        'Hide the explanation about default changes in Heap Dump Explorer',
      schema: z.boolean(),
      defaultValue: false,
    });

    const defaultFlamegraph = HeapDumpExplorerPlugin.defaultFlamegraphSetting;

    this.session = new HeapDumpExplorerSession(
      trace,
      trace.engine,
      hideDefaultChangedHint,
      defaultFlamegraph,
      store,
    );
  }

  // Change which heap dump we're looking at from another plugin
  updateHeadDump(upid: number, ts: time): void {
    const dump = this.session.dumps.find((d) => d.upid === upid && d.ts === ts);
    if (!dump) return;
    this.session.selectDump(dump);
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    const res = await ctx.engine.query(
      'SELECT count(*) AS cnt FROM heap_graph LIMIT 1',
    );
    if (res.iter({cnt: NUM}).cnt === 0) return;

    const session = this.session;
    const restored = await session.loadDumps();

    ctx.pages.registerPage({
      route: '/heapdump',
      render: (subpage) => m(HeapDumpPage, {session, subpage}),
    });

    if (restored) {
      // Restored from a shared link: land on the saved tab (beats the
      // default-open hint below).
      const sub = session.navPath;
      ctx.initialPage.suggest(sub ? `/heapdump/${sub}` : '/heapdump', 200);
    } else if (
      HeapProfilePlugin.openHeapDumpExplorerByDefaultFlag.get() &&
      !(await traceHasTimelineData(ctx))
    ) {
      session.autoNavigated = true;
      ctx.initialPage.suggest('/heapdump', 100);
    }

    ctx.plugins
      .getPlugin(HeapProfilePlugin)
      .registerOnNodeSelectedListener(({pathHashes, isDominator, upid, ts}) =>
        session.openFlamegraph({pathHashes, isDominator, upid, ts}),
      );

    ctx.sidebar.addMenuItem({
      section: 'current_trace',
      sortOrder: 30,
      text: 'Heapdump Explorer',
      href: '#!/heapdump',
      icon: 'memory',
    });
  }
}
