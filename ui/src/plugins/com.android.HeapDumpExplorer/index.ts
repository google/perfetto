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
import {z} from 'zod';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {NUM} from '../../trace_processor/query_result';
import HeapProfilePlugin from '../dev.perfetto.HeapProfile';
import {HeapDumpPage} from './heap_dump_page';
import {HeapDumpExplorerSession} from './session';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.HeapDumpExplorer';
  static readonly dependencies = [HeapProfilePlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const hideDefaultChangedHint = ctx.settings.register({
      id: 'com.android.HideHeapDumpExplorerDefaultChangedHint',
      name: 'Hide Heap Dump Explorer Explanation',
      description:
        'Hide the explanation about default changes in Heap Dump Explorer',
      schema: z.boolean(),
      defaultValue: false,
    });

    const res = await ctx.engine.query(
      'SELECT count(*) AS cnt FROM heap_graph_object LIMIT 1',
    );
    if (res.iter({cnt: NUM}).cnt === 0) return;

    const session = new HeapDumpExplorerSession(
      ctx,
      ctx.engine,
      hideDefaultChangedHint,
    );
    await session.loadDumps();

    ctx.pages.registerPage({
      route: '/heapdump',
      render: (subpage) => m(HeapDumpPage, {session, subpage}),
    });

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

    if (!(await traceHasTimelineData(ctx))) {
      session.autoNavigated = true;
      ctx.onTraceReady.addListener(() => ctx.navigate('#!/heapdump'));
    }
  }
}

// Returns true if the trace contains any timeline data the user is likely to
// want to inspect alongside a Java heap dump. When this is true we leave the
// user on the timeline; when false the trace is assumed to be a
// heap-dump-only capture and we auto-navigate to the Heap Dump Explorer.
async function traceHasTimelineData(ctx: Trace): Promise<boolean> {
  const res = await ctx.engine.query(`
    SELECT
      EXISTS(SELECT 1 FROM slice) OR
      EXISTS(SELECT 1 FROM sched) OR
      EXISTS(SELECT 1 FROM heap_profile_allocation) OR
      EXISTS(SELECT 1 FROM perf_sample)
      AS res
  `);
  return res.firstRow({res: NUM}).res > 0;
}
