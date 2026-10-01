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
import * as queries from './queries';
import {dumpKey, makeHref} from './nav';
import {assertExists} from '../../base/assert';
import {Callout} from '../../widgets/callout';
import {Anchor} from '../../widgets/anchor';
import {Box} from '../../widgets/box';

const PLUGIN_ID = 'com.android.HeapDumpExplorer';

export default class HeapDumpExplorerPlugin implements PerfettoPlugin {
  static readonly id = PLUGIN_ID;
  static readonly dependencies = [HeapProfilePlugin];
  private static defaultFlamegraphSetting: Setting<boolean>;
  private static hideDefaultChangedHintSetting: Setting<boolean>;

  static onActivate(app: App) {
    HeapDumpExplorerPlugin.defaultFlamegraphSetting = app.settings.register({
      id: 'com.android.HeapDumpExplorerDefaultFlamegraph',
      name: 'Heap Dump Explorer: Default to Flamegraph',
      description:
        'Make the flamegraph the first selected tab rather than the overview page in Heap Dump Explorer',
      schema: z.boolean(),
      defaultValue: false,
    });

    HeapDumpExplorerPlugin.hideDefaultChangedHintSetting =
      app.settings.register({
        id: 'com.android.HideHeapDumpExplorerDefaultChangedHint',
        name: 'Hide Heap Dump Explorer Explanation',
        description:
          'Hide the explanation about default changes in Heap Dump Explorer',
        schema: z.boolean(),
        defaultValue: false,
      });
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    if (!(await traceHasHeapGraph(ctx))) return;

    // Persistent state: the current subpage and per-dump panel states.
    const store = ctx.mountStore(PLUGIN_ID, migrateHdeState);

    const dumps = await queries.loadDumpsList(ctx.engine);

    // Whether the trace has HPROF field values (string contents, array data,
    // bitmap pixels), which some views need.
    const hasFieldValues = await queries.traceHasFieldValues(ctx.engine);

    // Shared by the views: trace-derived data plus per-dump panel state.
    const session = new HeapDumpExplorerSession(
      ctx,
      store,
      dumps,
      hasFieldValues,
    );

    let autoNavigated = false;
    const restoredSubpage = store.state.subpage;
    if (restoredSubpage !== undefined) {
      // Restored from a shared link: land on the saved subpage (beats the
      // default-open hint below).
      ctx.initialPage.suggest(`/heapdump${restoredSubpage}`, 300);
    } else if (
      HeapProfilePlugin.openHeapDumpExplorerByDefaultFlag.get() &&
      !(await traceHasTimelineData(ctx))
    ) {
      autoNavigated = true;
      // Open the first dump on the flamegraph or the overview, per the
      // DefaultFlamegraph setting.
      const firstDump = dumps.at(0);
      assertExists(firstDump);
      const view = HeapDumpExplorerPlugin.defaultFlamegraphSetting.get()
        ? 'flamegraph'
        : 'overview';
      const initialRoute = `/heapdump/${dumpKey(firstDump)}/${view}`;
      ctx.initialPage.suggest(initialRoute, 100);
    }

    ctx.pages.registerPage({
      route: '/heapdump',
      render: (subpage) => {
        // Keep the store's subpage in sync with the current URL.
        store.edit((s) => {
          s.subpage = subpage;
        });
        const hideHint = HeapDumpExplorerPlugin.hideDefaultChangedHintSetting;
        return m(
          '.pf-hde-root',
          autoNavigated &&
            !hideHint.get() &&
            renderDefaultChangedHint(() => hideHint.set(true)),
          m(HeapDumpPage, {session, subpage}),
        );
      },
    });

    ctx.plugins
      .getPlugin(HeapProfilePlugin)
      .registerOnNodeSelectedListener(({pathHashes, isDominator, upid, ts}) => {
        ctx.navigate(
          makeHref(
            {upid, ts},
            {view: 'flamegraph-objects', pathHashes, isDominator},
          ),
        );
      });

    ctx.sidebar.addMenuItem({
      section: 'current_trace',
      sortOrder: 30,
      text: 'Heapdump Explorer',
      href: '#!/heapdump',
      icon: 'memory',
    });
  }
}

// Whether the trace contains any heap graph data.
async function traceHasHeapGraph(trace: Trace): Promise<boolean> {
  const res = await trace.engine.query(
    'SELECT count(*) AS cnt FROM heap_graph LIMIT 1',
  );
  return res.iter({cnt: NUM}).cnt > 0;
}

// Explains that HDE opened by default (instead of the timeline), with a way
// back.
function renderDefaultChangedHint(onDismiss: () => void): m.Children {
  return m(Box, [
    m(
      Callout,
      {
        className: 'pf-hde-default-changed-callout',
        icon: 'info',
        dismissible: true,
        onDismiss,
      },
      m('p', [
        m(
          'span',
          'Heapdump Explorer is now the default view for traces ' +
            'with heap-graph data.',
        ),
        m(Anchor, {href: '#!/viewer'}, 'Back to Timeline'),
      ]),
    ),
  ]);
}
