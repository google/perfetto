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
import {assertExists} from '../../base/assert';
import {AsyncMemo} from '../../base/async_memo';
import {Time} from '../../base/time';
import {formatDuration} from '../../components/time_utils';
import type {Engine} from '../../trace_processor/engine';
import {NUM} from '../../trace_processor/query_result';
import {Button, ButtonVariant} from '../../widgets/button';
import {EmptyState} from '../../widgets/empty_state';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {Router} from '../../widgets/router';
import {TabStrip} from '../../widgets/tab_strip';
import {fmtHex, SQL_PREAMBLE} from './components';
import {dumpKey, makeHref, StaticHdeLink} from './nav';
import * as queries from './queries';
import type {HeapDumpExplorerSession} from './session';
import {AllObjectsView} from './views/all_objects_view';
import {ArraysView} from './views/arrays_view';
import {BitmapGalleryView} from './views/bitmap_gallery_view';
import {CallstackView} from './views/callstack_view';
import {ClassesView} from './views/classes_view';
import {DominatorsView} from './views/dominators_view';
import {
  FlamegraphObjectsView,
  flamegraphQuery,
} from './views/flamegraph_objects_view';
import {FlamegraphView} from './views/flamegraph_view';
import {ObjectView} from './views/object_view';
import {OverviewView} from './views/overview_view';
import {StringsView} from './views/strings_view';

interface HeapDumpPageAttrs {
  readonly session: HeapDumpExplorerSession;
  readonly subpage: string | undefined;
}

export class HeapDumpPage implements m.ClassComponent<HeapDumpPageAttrs> {
  view({attrs}: m.Vnode<HeapDumpPageAttrs>) {
    const {session} = attrs;
    const {trace, dumps} = session;
    const {engine} = trace;

    if (dumps.length === 0) {
      return m(
        '.pf-hde-page',
        m(EmptyState, {title: 'No heap dumps', fillHeight: true}),
      );
    }

    return m('.pf-hde-page', [
      m(Router, {
        path: attrs.subpage ?? '',
        fallback: () => m(EmptyState, {title: 'Page not found'}),
        routes: {
          '': () => {
            // No dump specified - pick the first one and show the default view.
            const firstDump = session.dumps.at(0);
            assertExists(firstDump);
            return renderPage(session, firstDump, 'overview', () =>
              m(OverviewView, {session, dump: firstDump}),
            );
          },
          ':dump': ({params}) => {
            return renderPage(session, params.dump, 'overview', (dump) =>
              m(OverviewView, {session, dump}),
            );
          },
          ':dump/overview': ({params}) => {
            return renderPage(session, params.dump, 'overview', (dump) =>
              m(OverviewView, {session, dump}),
            );
          },
          ':dump/flamegraph': ({params}) => {
            return renderPage(session, params.dump, 'flamegraph', (dump) =>
              m(FlamegraphView, {session, dump}),
            );
          },
          ':dump/classes': ({params}) => {
            return renderPage(session, params.dump, 'classes', (dump) =>
              m(ClassesView, {engine, dump: dump}),
            );
          },
          ':dump/classes/:root': ({params}) => {
            return renderPage(session, params.dump, 'classes', (dump) =>
              m(ClassesView, {
                engine,
                dump,
                rootClass: params.root,
              }),
            );
          },
          ':dump/objects': ({params}) => {
            return renderPage(session, params.dump, 'objects', (dump) =>
              m(AllObjectsView, {
                engine,
                dump,
              }),
            );
          },
          ':dump/objects/:cls': ({params}) => {
            return renderPage(session, params.dump, 'objects', (dump) =>
              m(AllObjectsView, {
                engine,
                dump,
                cls: params.cls,
              }),
            );
          },
          ':dump/dominators': ({params}) => {
            return renderPage(session, params.dump, 'dominators', (dump) =>
              m(DominatorsView, {
                engine,
                dump,
              }),
            );
          },
          ':dump/bitmaps': ({params}) => {
            return renderPage(session, params.dump, 'bitmaps', (dump) =>
              m(BitmapGalleryView, {
                engine,
                dump,
                hasFieldValues: session.hasFieldValues,
              }),
            );
          },
          ':dump/bitmaps/:filterKey': ({params}) => {
            return renderPage(session, params.dump, 'bitmaps', (dump) =>
              m(BitmapGalleryView, {
                engine,
                dump,
                hasFieldValues: session.hasFieldValues,
                filterKey: params.filterKey,
              }),
            );
          },
          ':dump/strings': ({params}) => {
            return renderPage(session, params.dump, 'strings', (dump) =>
              m(StringsView, {
                engine,
                dump,
                hasFieldValues: session.hasFieldValues,
              }),
            );
          },
          ':dump/strings/:q': ({params}) => {
            return renderPage(session, params.dump, 'strings', (dump) =>
              m(StringsView, {
                engine,
                dump,
                hasFieldValues: session.hasFieldValues,
                q: params.q,
              }),
            );
          },
          ':dump/arrays': ({params}) => {
            return renderPage(session, params.dump, 'arrays', (dump) =>
              m(ArraysView, {
                engine,
                dump,
                hasFieldValues: session.hasFieldValues,
              }),
            );
          },
          ':dump/arrays/:arrayHash': ({params}) => {
            return renderPage(session, params.dump, 'arrays', (dump) =>
              m(ArraysView, {
                engine,
                dump,
                hasFieldValues: session.hasFieldValues,
                arrayHash: params.arrayHash,
              }),
            );
          },
          ':dump/callstack': ({params}) => {
            return renderPage(session, params.dump, 'callstack', (dump) =>
              renderCallstack(session, dump),
            );
          },
          ':dump/object/:id': ({params}) => {
            const id = Number(params.id);
            return renderPage(
              session,
              params.dump,
              {
                title: m(ObjectTabTitle, {engine, id}),
              },
              (dump) => m(ObjectView, {session, dump, id}),
            );
          },
          ':dump/flamegraph-objects/:pathHashes': ({params}) =>
            renderFlamegraphObjectsPage(
              session,
              params.dump,
              params.pathHashes,
              false,
            ),
          ':dump/dominator-objects/:pathHashes': ({params}) =>
            renderFlamegraphObjectsPage(
              session,
              params.dump,
              params.pathHashes,
              true,
            ),
        },
      }),
    ]);
  }
}

function renderFlamegraphObjectsPage(
  session: HeapDumpExplorerSession,
  dump: string,
  pathHashes: string,
  isDominator: boolean,
): m.Children {
  const engine = session.trace.engine;
  return renderPage(
    session,
    dump,
    {
      title: m(FlamegraphTabTitle, {
        engine,
        pathHashes,
        isDominator,
      }),
    },
    (dump) =>
      m(FlamegraphObjectsView, {
        engine,
        dump,
        pathHashes,
        isDominator,
      }),
  );
}

// A tab that only exists while its URL is showing (an object inspector or a
// flamegraph drill-down).
interface EphemeralTab {
  readonly title: m.Children;
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

// The chrome (dump selector, tab bar) around a view of `dump`.
function renderPage(
  session: HeapDumpExplorerSession,
  dump: queries.HeapDump | string,
  activeTab: string | EphemeralTab,
  render: (dump: queries.HeapDump) => m.Children,
): m.Children {
  if (typeof dump === 'string') {
    const parsedDump = session.findDump(dump);
    if (!parsedDump) return renderMissingDumpPage(dump);
    dump = parsedDump;
  }

  const mkAttrs = (view: StaticHdeLink['view']) => ({
    key: view,
    href: makeHref(dump, {view}),
    active: activeTab === view,
  });

  // Keyed so Mithril remounts the views (and their SQLDataSources) when a
  // route is revisited with a different dump. Wrapped in an array as a
  // keyed fragment can't sit among unkeyed siblings.
  return [
    m.fragment({key: dumpKey(dump)}, [
      renderDumpSelector(session, dump),
      m(
        'main.pf-hde-page__tabs',
        m(TabStrip, [
          m(TabStrip.Link, mkAttrs('overview'), 'Overview'),
          m(TabStrip.Link, mkAttrs('flamegraph'), 'Flamegraph'),
          m(TabStrip.Link, mkAttrs('classes'), 'Classes'),
          m(TabStrip.Link, mkAttrs('objects'), 'Objects'),
          m(TabStrip.Link, mkAttrs('dominators'), 'Dominators'),
          m(TabStrip.Link, mkAttrs('bitmaps'), 'Bitmaps'),
          m(TabStrip.Link, mkAttrs('strings'), 'Strings'),
          m(TabStrip.Link, mkAttrs('arrays'), 'Arrays'),
          m(TabStrip.Link, mkAttrs('callstack'), 'Callstack'),
          typeof activeTab !== 'string' &&
            m(TabStrip.Link, {key: 'ephemeral', active: true}, activeTab.title),
        ]),
        m('.pf-hde-page__content', render(dump)),
      ),
    ]),
  ];
}

function renderMissingDumpPage(key: string) {
  return m(EmptyState, {title: `Heap dump ${key} not found`, fillHeight: true});
}
