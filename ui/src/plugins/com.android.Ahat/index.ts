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
import {SidebarManager} from '../../public/sidebar';
import {NUM} from '../../trace_processor/query_result';
import {HeapDumpPage} from './heap_dump_page';
import {nav} from './nav_state';
import {resetBitmapDumpDataCache} from './queries';

let sidebarManager: SidebarManager;

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.Ahat';

  static onActivate(app: App): void {
    sidebarManager = app.sidebar;
    app.pages.registerPage({
      route: '/ahat',
      render: (subpage) => m(HeapDumpPage, {subpage}),
    });
  }

  constructor(_trace: Trace) {}

  async onTraceLoad(ctx: Trace): Promise<void> {
    // Check if this trace contains heap graph data.
    const res = await ctx.engine.query(
      'SELECT count(*) AS cnt FROM heap_graph_object LIMIT 1',
    );
    const cnt = res.iter({cnt: NUM}).cnt;
    if (cnt === 0) return;

    // Materialize the dominator tree for all subsequent queries.
    await ctx.engine.query(
      'INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree',
    );

    // Make the engine available to the page component.
    HeapDumpPage.engine = ctx.engine;
    HeapDumpPage.hasHeapData = true;
    resetBitmapDumpDataCache();

    // Register sidebar items under the Ahat section. Disposed on trace unload.
    const viewItems = [
      {label: 'Overview', subpage: '', view: 'overview', icon: 'dashboard'},
      {
        label: 'Allocations',
        subpage: 'allocations',
        view: 'allocations',
        icon: 'bar_chart',
      },
      {
        label: 'Rooted',
        subpage: 'rooted',
        view: 'rooted',
        icon: 'account_tree',
      },
      {label: 'Bitmaps', subpage: 'bitmaps', view: 'bitmaps', icon: 'image'},
      {
        label: 'Strings',
        subpage: 'strings',
        view: 'strings',
        icon: 'text_fields',
      },
      {label: 'Search', subpage: 'search', view: 'search', icon: 'search'},
    ];
    for (const v of viewItems) {
      ctx.trash.use(
        sidebarManager.addMenuItem({
          section: 'ahat',
          text: v.label,
          href: v.subpage ? `#!/ahat/${v.subpage}` : '#!/ahat',
          icon: v.icon,
          cssClass: () => (nav.view === v.view ? 'ah-sidebar-active' : ''),
        }),
      );
    }
  }
}
