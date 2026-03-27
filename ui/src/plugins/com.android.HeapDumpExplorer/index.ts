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
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {App} from '../../public/app';
import {NUM} from '../../trace_processor/query_result';
import HeapProfilePlugin from '../dev.perfetto.HeapProfile';
import {
  HeapDumpPage,
  setFlamegraphSelection,
  resetFlamegraphSelection,
  resetCachedOverview,
} from './heap_dump_page';
import {resetBitmapDumpDataCache} from './queries';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.HeapDumpExplorer';
  static readonly dependencies = [HeapProfilePlugin];

  static onActivate(app: App): void {
    app.pages.registerPage({
      route: '/heapdump',
      render: (subpage) => m(HeapDumpPage, {subpage}),
    });
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    const res = await ctx.engine.query(
      'SELECT count(*) AS cnt FROM heap_graph_object LIMIT 1',
    );
    const cnt = res.iter({cnt: NUM}).cnt;
    if (cnt === 0) return;

    HeapDumpPage.engine = ctx.engine;
    HeapDumpPage.trace = ctx;
    HeapDumpPage.hasHeapData = true;
    resetBitmapDumpDataCache();
    resetFlamegraphSelection();
    resetCachedOverview();

    ctx.plugins
      .getPlugin(HeapProfilePlugin)
      .registerOnNodeSelectedListener(({pathHashes, isDominator}) =>
        setFlamegraphSelection({pathHashes, isDominator}),
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
