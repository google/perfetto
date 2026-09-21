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

// Heap Dump Explorer URL scheme
// =============================
// This plugin registers a single page at route `/heapdump`; every tab is a
// subpage under it:
//
//     https://ui.perfetto.dev/#!/heapdump/<subpage>
//
// `#!/heapdump` with no subpage defaults to Overview (or Flamegraph when the
// "Default to Flamegraph" setting is on).
//
// Params are encoded two ways. The router (core/router.ts) only round-trips the
// PATH portion of the subpage; a query string after the subpage is parsed into
// route args and is NOT passed to this page. So:
//   - path-encoded params  -> survive the live URL / address bar / back-forward
//   - query-encoded params -> stripped from the live route; they only take
//     effect when restored from a permalink (the `s` state arg) or via in-app
//     navigation. See stateToSubpage()/subpageToState() in nav_state.ts (the
//     single source of truth for (de)serialization).
//
// Per-view schemas (param key -> NavState field -> view attr consumed):
//
//   Overview            #!/heapdump/overview
//   Flamegraph          #!/heapdump/flamegraph
//   Dominators          #!/heapdump/dominators
//
//   Classes             #!/heapdump/classes[?root=<class>]
//                       root -> rootClass -> ClassesView.initialRootClass
//                       (query-encoded)
//
//   Objects             #!/heapdump/objects[<class>]
//                       <class> -> cls -> AllObjectsView.initialClass
//                       (path-encoded)
//
//   Object (instance)   #!/heapdump/object_<id>
//                       <id> (hex 0x.. or decimal) -> id -> ObjectView.params.id
//                       (path-encoded; the tab `label` is never in the URL)
//
//   Bitmaps             #!/heapdump/bitmaps[<id>][?fk=<filterKey>]
//                       <id> (hex/dec) -> id; fk -> filterKey
//                       -> BitmapGalleryView.filterKey (path + query-encoded)
//
//   Strings             #!/heapdump/strings[?q=<value>]
//                       q -> q -> StringsView.initialQuery (query-encoded)
//
//   Arrays              #!/heapdump/arrays[?ah=<arrayHash>]
//                       ah -> arrayHash -> ArraysView.initialArrayHash
//                       (query-encoded)
//
//   Flamegraph objects  #!/heapdump/flamegraph_objects[<0|1>_<pathHashes>]
//                       <0|1> -> isDominator; <pathHashes> (CSV) -> pathHashes
//                       -> FlamegraphObjectsView (path-encoded; bare
//                       `flamegraph_objects` means no selection yet)
//
// Examples:
//
//   No-param views:
//     #!/heapdump/overview
//     #!/heapdump/flamegraph
//     #!/heapdump/dominators
//
//   Path-encoded (work in the address bar):
//     #!/heapdump/objects/java.lang.String
//     #!/heapdump/objects/com.example.Foo
//     #!/heapdump/object/0x1a2b3c          (hex id)
//     #!/heapdump/object/172938            (decimal id also parses)
//     #!/heapdump/bitmaps/0x2c3d4e
//     #!/heapdump/flamegraph_objects/0/a1b2c3d4,e5f6a7b8   (non-dominator)
//     #!/heapdump/flamegraph_objects/1/a1b2c3d4,e5f6a7b8   (dominator)
//     #!/heapdump/flamegraph_objects       (tab with no selection yet)
//
//   Query-encoded (only via permalink / in-app nav):
//     #!/heapdump/classes?root=com.example.Foo
//     #!/heapdump/strings?q=hello%20world
//     #!/heapdump/arrays?ah=<array_hash>
//     #!/heapdump/bitmaps?fk=<buffer_hash>
//     #!/heapdump/bitmaps/0x2c3d4e?fk=<buffer_hash>
//
// Permalink form: the full state (active dump, this `nav` string, open
// flamegraph/instance tabs, flamegraph & callstack panel state) is serialized
// into the `s` route arg (see persisted_state.ts). The `nav` field stores the
// complete stateToSubpage string *including* the query part, which is what lets
// query-encoded params survive a restore:
//   https://ui.perfetto.dev/?s=<base64-state>&#!/heapdump/classes?root=com.example.Foo
//   https://ui.perfetto.dev/?s=<base64-state>&#!/heapdump/strings?q=hello%20world

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

const PLUGIN_ID = 'com.android.HeapDumpExplorer';

export default class HeapDumpExplorerPlugin implements PerfettoPlugin {
  static readonly id = PLUGIN_ID;
  static readonly dependencies = [HeapProfilePlugin];
  private static defaultFlamegraphSetting: Setting<boolean>;

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

  async onTraceLoad(ctx: Trace): Promise<void> {
    const hideDefaultChangedHint = ctx.settings.register({
      id: 'com.android.HideHeapDumpExplorerDefaultChangedHint',
      name: 'Hide Heap Dump Explorer Explanation',
      description:
        'Hide the explanation about default changes in Heap Dump Explorer',
      schema: z.boolean(),
      defaultValue: false,
    });

    const defaultFlamegraph = HeapDumpExplorerPlugin.defaultFlamegraphSetting;

    const res = await ctx.engine.query(
      'SELECT count(*) AS cnt FROM heap_graph LIMIT 1',
    );
    if (res.iter({cnt: NUM}).cnt === 0) return;

    // The core restores this store (phase 1) before plugins run, so the session
    // reads any shared-link state straight from it.
    const store = ctx.mountStore(PLUGIN_ID, migrateHdeState);

    const session = new HeapDumpExplorerSession(
      ctx,
      ctx.engine,
      hideDefaultChangedHint,
      defaultFlamegraph,
      store,
    );
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
