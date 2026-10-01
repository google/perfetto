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
import {Time} from '../../base/time';
import {AsyncMemo} from '../../base/async_memo';
import {Button, ButtonVariant} from '../../widgets/button';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {EmptyState} from '../../widgets/empty_state';
import {Router} from '../../widgets/router';
import {TabBar, TabBarLink} from '../../widgets/tab_bar';
import {formatDuration} from '../../components/time_utils';
import * as queries from './queries';
import {dumpKey, type HdeLink, makeHref} from './nav';
import {OverviewView} from './views/overview_view';
import {DominatorsView} from './views/dominators_view';
import {ObjectView} from './views/object_view';
import {AllObjectsView} from './views/all_objects_view';
import {BitmapGalleryView} from './views/bitmap_gallery_view';
import {ClassesView} from './views/classes_view';
import {StringsView} from './views/strings_view';
import {ArraysView} from './views/arrays_view';
import {
  FlamegraphObjectsView,
  flamegraphQuery,
} from './views/flamegraph_objects_view';
import {FlamegraphView} from './views/flamegraph_view';
import {CallstackView} from './views/callstack_view';
import type {HeapDumpExplorerSession} from './session';
import {assertExists} from '../../base/assert';
import type {Engine} from '../../trace_processor/engine';
import {NUM} from '../../trace_processor/query_result';
import {SQL_PREAMBLE, fmtHex} from './components';

// The fixed (non-ephemeral) views, in tab order.
const STATIC_VIEWS = [
  'overview',
  'flamegraph',
  'classes',
  'objects',
  'dominators',
  'bitmaps',
  'strings',
  'arrays',
  'callstack',
] as const;

type StaticView = (typeof STATIC_VIEWS)[number];

const STATIC_VIEW_TITLES: Record<StaticView, string> = {
  overview: 'Overview',
  flamegraph: 'Flamegraph',
  classes: 'Classes',
  objects: 'Objects',
  dominators: 'Dominators',
  bitmaps: 'Bitmaps',
  strings: 'Strings',
  arrays: 'Arrays',
  callstack: 'Callstack',
};

interface HeapDumpPageAttrs {
  readonly session: HeapDumpExplorerSession;
  readonly subpage: string | undefined;
}

export class HeapDumpPage implements m.ClassComponent<HeapDumpPageAttrs> {
  view({attrs}: m.Vnode<HeapDumpPageAttrs>) {
    const {session} = attrs;
    if (session.dumps.length === 0) {
      return m(
        '.pf-hde-page',
        m(EmptyState, {title: 'No heap dumps', fillHeight: true}),
      );
    }

    const engine = session.trace.engine;

    const renderDefaultView = (dump: queries.HeapDump) =>
      renderDump(session, dump, 'overview', renderOverview(session, dump));

    return m(
      '.pf-hde-page',
      m(Router, {
        path: attrs.subpage,
        fallback: () => m(EmptyState, {title: 'Page not found'}),
        routes: {
          '': () => {
            // No dump specified - pick the first one and show the default view.
            const firstDump = session.dumps.at(0);
            assertExists(firstDump);
            return renderDefaultView(firstDump);
          },
          ':dump': ({params}) => {
            return renderDumpFromKey(session, params.dump, 'overview', (dump) =>
              renderOverview(session, dump),
            );
          },
          ':dump/overview': ({params}) => {
            return renderDumpFromKey(session, params.dump, 'overview', (dump) =>
              renderOverview(session, dump),
            );
          },
          ':dump/flamegraph': ({params}) => {
            return renderDumpFromKey(
              session,
              params.dump,
              'flamegraph',
              (dump) => renderFlamegraph(session, dump),
            );
          },
          ':dump/classes': ({params}) => {
            return renderDumpFromKey(session, params.dump, 'classes', (dump) =>
              m(ClassesView, {engine, activeDump: dump}),
            );
          },
          ':dump/classes/:root': ({params}) => {
            return renderDumpFromKey(session, params.dump, 'classes', (dump) =>
              m(ClassesView, {
                engine,
                activeDump: dump,
                rootClass: params.root,
              }),
            );
          },
          ':dump/objects': ({params}) => {
            return renderDumpFromKey(session, params.dump, 'objects', (dump) =>
              m(AllObjectsView, {
                engine,
                activeDump: dump,
              }),
            );
          },
          ':dump/objects/:cls': ({params}) => {
            return renderDumpFromKey(session, params.dump, 'objects', (dump) =>
              m(AllObjectsView, {
                engine,
                activeDump: dump,
                cls: params.cls,
              }),
            );
          },
          ':dump/dominators': ({params}) => {
            return renderDumpFromKey(
              session,
              params.dump,
              'dominators',
              (dump) =>
                m(DominatorsView, {
                  engine,
                  activeDump: dump,
                }),
            );
          },
          ':dump/bitmaps': ({params}) => {
            return renderDumpFromKey(session, params.dump, 'bitmaps', (dump) =>
              m(BitmapGalleryView, {
                engine,
                activeDump: dump,
                hasFieldValues: session.hasFieldValues,
              }),
            );
          },
          ':dump/bitmaps/:filterKey': ({params}) => {
            return renderDumpFromKey(session, params.dump, 'bitmaps', (dump) =>
              m(BitmapGalleryView, {
                engine,
                activeDump: dump,
                hasFieldValues: session.hasFieldValues,
                filterKey: params.filterKey,
              }),
            );
          },
          ':dump/strings': ({params}) => {
            return renderDumpFromKey(session, params.dump, 'strings', (dump) =>
              m(StringsView, {
                engine,
                activeDump: dump,
                hasFieldValues: session.hasFieldValues,
              }),
            );
          },
          ':dump/strings/:q': ({params}) => {
            return renderDumpFromKey(session, params.dump, 'strings', (dump) =>
              m(StringsView, {
                engine,
                activeDump: dump,
                hasFieldValues: session.hasFieldValues,
                q: params.q,
              }),
            );
          },
          ':dump/arrays': ({params}) => {
            return renderDumpFromKey(session, params.dump, 'arrays', (dump) =>
              m(ArraysView, {
                engine,
                activeDump: dump,
                hasFieldValues: session.hasFieldValues,
              }),
            );
          },
          ':dump/arrays/:arrayHash': ({params}) => {
            return renderDumpFromKey(session, params.dump, 'arrays', (dump) =>
              m(ArraysView, {
                engine,
                activeDump: dump,
                hasFieldValues: session.hasFieldValues,
                arrayHash: params.arrayHash,
              }),
            );
          },
          ':dump/callstack': ({params}) => {
            return renderDumpFromKey(
              session,
              params.dump,
              'callstack',
              (dump) => renderCallstack(session, dump),
            );
          },
          ':dump/object/:id': ({params}) => {
            const id = Number(params.id);
            return renderDumpFromKey(
              session,
              params.dump,
              {
                link: {view: 'object', id},
                title: m(ObjectTabTitle, {engine, id}),
              },
              (dump) => renderObject(session, dump, id),
            );
          },
          ':dump/flamegraph-objects/:pathHashes': ({params}) => {
            return renderDumpFromKey(
              session,
              params.dump,
              flamegraphObjectsTab(session, params.pathHashes, false),
              (dump) =>
                renderFlamegraphObjects(
                  session,
                  dump,
                  params.pathHashes,
                  false,
                ),
            );
          },
          ':dump/dominator-objects/:pathHashes': ({params}) => {
            return renderDumpFromKey(
              session,
              params.dump,
              flamegraphObjectsTab(session, params.pathHashes, true),
              (dump) =>
                renderFlamegraphObjects(session, dump, params.pathHashes, true),
            );
          },
        },
      }),
    );
  }
}

// A tab that only exists while its URL is showing (an object inspector or a
// flamegraph drill-down).
interface EphemeralTab {
  readonly link: HdeLink;
  readonly title: m.Children;
}

// Which tab is showing: a static view, or an ephemeral tab.
type ActiveTab = StaticView | EphemeralTab;

function renderTabBar(
  session: HeapDumpExplorerSession,
  dump: queries.HeapDump,
  activeTab: ActiveTab,
): m.Children {
  return m(TabBar, [
    STATIC_VIEWS.map((view) =>
      m(
        TabBarLink,
        {
          key: view,
          href: makeHref(dump, {view}),
          active: activeTab === view,
        },
        STATIC_VIEW_TITLES[view],
      ),
    ),
    typeof activeTab !== 'string'
      ? [
          m(
            TabBarLink,
            {
              key: 'ephemeral',
              href: makeHref(dump, activeTab.link),
              active: true,
              onClose: () =>
                session.trace.navigate(makeHref(dump, {view: 'overview'})),
            },
            activeTab.title,
          ),
        ]
      : [],
  ]);
}

interface ObjectTabTitleAttrs {
  readonly engine: Engine;
  readonly id: number;
}

// An object tab's title: the object's display name once fetched.
class ObjectTabTitle implements m.ClassComponent<ObjectTabTitleAttrs> {
  private readonly displayMemo = new AsyncMemo<string | undefined>();

  onremove() {
    this.displayMemo.dispose();
  }

  view({attrs}: m.Vnode<ObjectTabTitleAttrs>) {
    const {data: display} = this.displayMemo.use({
      key: {id: attrs.id},
      compute: () => queries.getObjectDisplay(attrs.engine, attrs.id),
    });
    return display ?? `Object ${fmtHex(attrs.id)}`;
  }
}

interface FlamegraphTabTitleAttrs {
  readonly engine: Engine;
  readonly pathHashes: string;
  readonly isDominator: boolean;
}

// A flamegraph tab's title, with its object count once fetched.
class FlamegraphTabTitle implements m.ClassComponent<FlamegraphTabTitleAttrs> {
  private readonly countMemo = new AsyncMemo<number>();

  onremove() {
    this.countMemo.dispose();
  }

  view({attrs}: m.Vnode<FlamegraphTabTitleAttrs>) {
    const query = flamegraphQuery(attrs.pathHashes, attrs.isDominator);
    const {data: count} = this.countMemo.use({
      key: {query},
      compute: async () => {
        const res = await attrs.engine.query(
          `${SQL_PREAMBLE}; SELECT COUNT(*) AS c FROM (${query})`,
        );
        return res.firstRow({c: NUM}).c;
      },
    });
    return count !== undefined
      ? `Flamegraph objects (${count.toLocaleString()})`
      : 'Flamegraph objects';
  }
}

function flamegraphObjectsTab(
  session: HeapDumpExplorerSession,
  pathHashes: string,
  isDominator: boolean,
): EphemeralTab {
  return {
    link: {view: 'flamegraph-objects', pathHashes, isDominator},
    title: m(FlamegraphTabTitle, {
      engine: session.trace.engine,
      pathHashes,
      isDominator,
    }),
  };
}

function renderObject(
  session: HeapDumpExplorerSession,
  dump: queries.HeapDump,
  id: number,
): m.Children {
  return m(ObjectView, {
    engine: session.trace.engine,
    activeDump: dump,
    session,
    id,
  });
}

function renderFlamegraphObjects(
  session: HeapDumpExplorerSession,
  dump: queries.HeapDump,
  pathHashes: string,
  isDominator: boolean,
): m.Children {
  return m(FlamegraphObjectsView, {
    engine: session.trace.engine,
    dump,
    pathHashes,
    isDominator,
  });
}

function renderOverview(
  session: HeapDumpExplorerSession,
  dump: queries.HeapDump,
): m.Children {
  return m(OverviewView, {
    engine: session.trace.engine,
    hasFieldValues: session.hasFieldValues,
    activeDump: dump,
  });
}

function renderFlamegraph(
  session: HeapDumpExplorerSession,
  dump: queries.HeapDump,
): m.Children {
  return m(FlamegraphView, {
    trace: session.trace,
    upid: dump.upid,
    ts: dump.ts,
    state: session.flamegraphPanelState(dump),
    onStateChange: (state) => session.setFlamegraphPanelState(dump, state),
  });
}

function renderCallstack(
  session: HeapDumpExplorerSession,
  dump: queries.HeapDump,
): m.Children {
  return m(CallstackView, {
    trace: session.trace,
    dump,
    state: session.callstackPanelState(dump),
    onStateChange: (state) => session.setCallstackPanelState(dump, state),
  });
}

function processLabel(d: queries.HeapDump): string {
  return d.processName !== null
    ? `${d.processName} (pid ${d.pid})`
    : `pid ${d.pid}`;
}

// Switching dumps goes to the new dump's overview.
function renderDumpSelector(
  session: HeapDumpExplorerSession,
  active: queries.HeapDump,
): m.Children {
  const allDumps = session.dumps;
  if (allDumps.length <= 1) return null;

  return m(
    'div',
    {class: 'pf-hde-dump-selector'},
    m('span', {class: 'pf-hde-dump-selector__label'}, 'Heap dump:'),
    m(
      PopupMenu,
      {
        trigger: m(Button, {
          label: processLabel(active),
          icon: 'memory',
          rightIcon: 'arrow_drop_down',
          variant: ButtonVariant.Outlined,
          compact: true,
        }),
      },
      allDumps.map((d) => {
        const offset = Time.diff(d.ts, session.trace.traceInfo.start);
        return m(MenuItem, {
          label: `${processLabel(d)} — ${formatDuration(session.trace, offset)}`,
          active: d === active,
          onclick: () => session.trace.navigate(makeHref(d)),
        });
      }),
    ),
  );
}

function renderDumpFromKey(
  session: HeapDumpExplorerSession,
  key: string,
  activeTab: ActiveTab,
  content: (dump: queries.HeapDump) => m.Children,
): m.Children {
  const dump = session.findDump(key);
  if (!dump) return renderMissingDumpPage(key);
  return renderDump(session, dump, activeTab, content(dump));
}

// The chrome (dump selector, tab bar) around a view of `dump`.
function renderDump(
  session: HeapDumpExplorerSession,
  dump: queries.HeapDump,
  activeTab: ActiveTab,
  content: m.Children,
): m.Children {
  // Keyed so Mithril remounts the views (and their SQLDataSources) when a
  // route is revisited with a different dump. Wrapped in an array as a
  // keyed fragment can't sit among unkeyed siblings.
  return [
    m.fragment({key: dumpKey(dump)}, [
      renderDumpSelector(session, dump),
      m(
        'main.pf-hde-page__tabs',
        renderTabBar(session, dump, activeTab),
        m('.pf-hde-page__content', content),
      ),
    ]),
  ];
}

function renderMissingDumpPage(key: string) {
  return m(EmptyState, {title: `Heap dump ${key} not found`, fillHeight: true});
}
