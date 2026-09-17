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
import {Spinner} from '../../widgets/spinner';
import {Button, ButtonVariant} from '../../widgets/button';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {Tabs} from '../../widgets/tabs';
import type {TabsTab} from '../../widgets/tabs';
import {formatDuration} from '../../components/time_utils';
import type {DumpRouteRef, NavState, NavView} from './nav_state';
import type {OverviewData} from './types';
import * as queries from './queries';
import {OverviewView} from './views/overview_view';
import {DominatorsView} from './views/dominators_view';
import {ObjectView} from './views/object_view';
import {AllObjectsView} from './views/all_objects_view';
import {BitmapGalleryView} from './views/bitmap_gallery_view';
import {ClassesView} from './views/classes_view';
import {StringsView} from './views/strings_view';
import {ArraysView} from './views/arrays_view';
import {FlamegraphObjectsView} from './views/flamegraph_objects_view';
import {FlamegraphView} from './views/flamegraph_view';
import {CallstackView} from './views/callstack_view';
import type {HeapDumpExplorerSession} from './session';
import {generateNavLink, NavLink, parseNavLink} from './navigate';
import {AsyncMemo} from '../../base/async_memo';
import type {Trace} from '../../public/trace';
import {maybeUndefined} from '../../base/utils';

interface HeapDumpPageAttrs {
  readonly trace: Trace;
  readonly subpage: string | undefined;
  readonly allDumps: readonly queries.HeapDump[];
  readonly session: HeapDumpExplorerSession;
}

const FG_KEY_PREFIX = 'fg-';
const INSTANCE_KEY_PREFIX = 'inst-';

export class HeapDumpPage implements m.ClassComponent<HeapDumpPageAttrs> {
  private readonly overviewMemo = new AsyncMemo<OverviewData | undefined>();

  view({attrs}: m.Vnode<HeapDumpPageAttrs>): m.Children {
    const {trace, subpage, allDumps, session} = attrs;

    // // Mirror URL-driven nav (back/forward, address bar) into the store, and
    // // make sure any deep-linked object/flamegraph tab exists in the store.
    // session.syncFromSubpage(subpage);
    // session.syncInstanceTabFromNav();
    // session.syncFlamegraphTabFromNav();

    const navResult = parseNavLink(subpage, session.defaultView);

    // The URL pointed at a tab that doesn't exist (typo or stale deep link).
    // This is the hook for handling invalid tabs — decide what to do here.
    if (navResult.status === 'invalid_tab') {
      return m(
        '.pf-hde-page',
        m('.pf-hde-loading', `404: Unknown tab "${navResult.raw}"`),
      );
    }

    const nav = navResult.nav;
    const activeDumpResult = resolveActiveDump(allDumps, nav);

    if (activeDumpResult.status === 'dumpless') {
      return m(
        '.pf-hde-page',
        m('.pf-hde-loading', 'No dumps available in this trace'),
      );
    }

    if (activeDumpResult.status === 'no_exist') {
      return m(
        '.pf-hde-page',
        renderDumpSelector(trace, allDumps, undefined),
        m('.pf-hde-loading', "404: Heap dump doesn't exist"),
      );
    }

    const activeDump = activeDumpResult.dump;

    // Load the overview data
    const {isPending, data: overview} = this.overviewMemo.use({
      key: {activeDump},
      compute: () => queries.getOverview(trace.engine, activeDump),
    });

    if (isPending || overview === undefined) {
      return m(
        '.pf-hde-page',
        renderDumpSelector(trace, allDumps, activeDump),
        m('.pf-hde-loading', m(Spinner, {easing: true})),
      );
    }

    return m(
      '.pf-hde-page',
      renderDumpSelector(trace, allDumps, activeDump),
      m(
        '.pf-hde-page__tabs',
        m(Tabs, {
          tabs: [
            {
              key: 'overview',
              title: 'Overview',
              content: m(OverviewView, {
                overview,
                activeDump,
              }),
              onClick: () =>
                navigate(trace, {dump: activeDump, tab: 'overview'}),
              active: nav.tab === 'overview',
            },
            {
              key: 'flamegraph',
              title: 'Flamegraph',
              content: 'flamegraph data....',
              onClick: () =>
                navigate(trace, {dump: activeDump, tab: 'flamegraph'}),
              active: nav.tab === 'flamegraph',
            },
          ],
        }),
      ),
    );
  }
}

function navigate(trace: Trace, nav: NavLink): void {
  const link = generateNavLink(nav);
  trace.navigate(link);
}

type ActiveDumpResult =
  | {status: 'dumpless'}
  | {status: 'no_exist'}
  | {status: 'ok'; dump: DumpRouteRef};

function resolveActiveDump(
  allDumps: readonly queries.HeapDump[],
  nav: NavLink,
): ActiveDumpResult {
  const firstDump = maybeUndefined(allDumps[0]);

  // There are no dumps in this trace at all
  if (!firstDump) return {status: 'dumpless'};

  const dumpFromLink = nav.dump;

  if (dumpFromLink === undefined) {
    // No dump found in link - use the first dump instead
    location.replace(generateNavLink({...nav, dump: firstDump}));
    return {status: 'ok', dump: firstDump};
  }

  // Check the dump actually exists in the list of all dumps
  const dumpExists = allDumps.some(
    (d) => d.upid === dumpFromLink.upid && d.ts === dumpFromLink.ts,
  );

  if (!dumpExists) {
    return {status: 'no_exist'};
  }

  return {status: 'ok', dump: dumpFromLink};
}

function fgTabKey(pathHashes: string, isDominator: boolean): string {
  return `${FG_KEY_PREFIX}${isDominator ? 'd' : 'n'}:${pathHashes}`;
}

function instanceTabKey(objId: number): string {
  return `${INSTANCE_KEY_PREFIX}${objId}`;
}

function activeTabKey(session: HeapDumpExplorerSession, nav: NavLink): string {
  switch (nav.tab) {
    case 'flamegraph-objects': {
      const tabs = session.flamegraphTabs;
      const tab =
        (nav.pathHashes !== undefined &&
          tabs.find(
            (t) =>
              t.pathHashes === nav.pathHashes &&
              t.isDominator === (nav.isDominator ?? false),
          )) ||
        tabs[tabs.length - 1];
      return fgTabKey(tab.pathHashes, tab.isDominator);
    }
    case 'object':
      return instanceTabKey(nav.id);
    default:
      return nav.tab;
  }
}

// Per-tab select/close actions, looked up by key.
interface TabActions {
  select(): void;
  close?(): void;
}

function buildTabs(
  session: HeapDumpExplorerSession,
  activeDump: DumpRouteRef,
  state: NavState,
  overview: OverviewData,
): {tabs: TabsTab[]; actions: Map<string, TabActions>} {
  const {engine, trace, clearNavParam} = session;
  const hideExplanationSetting = session.hideDefaultChangedHint;
  const hideHint = hideExplanationSetting.get();
  const actions = new Map<string, TabActions>();

  const tabs: TabsTab[] = [
    {
      key: 'overview',
      title: 'Overview',
      content: m(OverviewView, {
        overview,
        activeDump,
        showDefaultChangedHint: session.autoNavigated && !hideHint,
        onBackToTimeline: () => trace.navigate('#!/viewer'),
        onDismissDefaultChangedHint: () => hideExplanationSetting.set(true),
      }),
    },
    {
      key: 'flamegraph',
      title: 'Flamegraph',
      content: m(FlamegraphView, {
        trace,
        upid: activeDump.upid,
        ts: activeDump.ts,
        state: session.flamegraphPanelState,
        onStateChange: session.setFlamegraphPanelState,
        onShowObjects: (pathHashes, isDominator) =>
          session.openFlamegraph({
            pathHashes,
            isDominator,
            upid: activeDump.upid,
            ts: activeDump.ts,
          }),
      }),
    },
    {
      key: 'classes',
      title: 'Classes',
      content: m(ClassesView, {
        engine,
        activeDump,
        clearNavParam,
        initialRootClass:
          state.view === 'classes' ? state.params.rootClass : undefined,
      }),
    },
    {
      key: 'objects',
      title: 'Objects',
      content: m(AllObjectsView, {
        engine,
        activeDump,
        clearNavParam,
        initialClass: state.view === 'objects' ? state.params.cls : undefined,
      }),
    },
    {
      key: 'dominators',
      title: 'Dominators',
      content: m(DominatorsView, {
        engine,
        activeDump,
      }),
    },
    {
      key: 'bitmaps',
      title: 'Bitmaps',
      content: m(BitmapGalleryView, {
        engine,
        activeDump,
        clearNavParam,
        hasFieldValues: overview.hasFieldValues,
        filterKey:
          state.view === 'bitmaps' ? state.params.filterKey : undefined,
      }),
    },
    {
      key: 'strings',
      title: 'Strings',
      content: m(StringsView, {
        engine,
        activeDump,
        clearNavParam,
        initialQuery: state.view === 'strings' ? state.params.q : undefined,
        hasFieldValues: overview.hasFieldValues,
      }),
    },
    {
      key: 'arrays',
      title: 'Arrays',
      content: m(ArraysView, {
        engine,
        activeDump,
        clearNavParam,
        initialArrayHash:
          state.view === 'arrays' ? state.params.arrayHash : undefined,
        hasFieldValues: overview.hasFieldValues,
      }),
    },
    {
      key: 'callstack',
      title: 'Callstack',
      content: m(CallstackView, {
        trace,
        dump: activeDump,
        state: session.callstackPanelState,
        onStateChange: session.setCallstackPanelState,
      }),
    },
  ];

  // Static tab keys are view names.
  for (const tab of tabs) {
    actions.set(tab.key, {select: () => session.navigate(tab.key as NavView)});
  }

  for (const fg of session.flamegraphTabs) {
    const key = fgTabKey(fg.pathHashes, fg.isDominator);
    const title =
      fg.count !== null
        ? `Flamegraph objects (${fg.count.toLocaleString()})`
        : 'Flamegraph objects';
    tabs.push({
      key,
      title,
      closeButton: true,
      content: m(FlamegraphObjectsView, {
        engine,
        activeDump,
        pathHashes: fg.pathHashes,
        isDominator: fg.isDominator,
        onBackToTimeline: () => trace.navigate('#!/viewer'),
      }),
    });
    actions.set(key, {
      select: () =>
        session.navigate('flamegraph-objects', {
          pathHashes: fg.pathHashes,
          isDominator: fg.isDominator,
        }),
      close: () => session.closeFlamegraph(fg.pathHashes, fg.isDominator),
    });
  }

  for (const obj of session.instanceTabs) {
    const key = instanceTabKey(obj.objId);
    tabs.push({
      key,
      title: obj.label,
      closeButton: true,
      content: m(ObjectView, {
        engine,
        activeDump,
        heaps: overview.heaps,
        openFlamegraphPivotedAt: session.openFlamegraphPivotedAt,
        params: {id: obj.objId},
      }),
    });
    actions.set(key, {
      select: () => session.navigate('object', {id: obj.objId}),
      close: () => session.closeInstanceTab(obj.objId),
    });
  }

  return {tabs, actions};
}

function processLabel(d: queries.HeapDump): string {
  return d.processName !== null
    ? `${d.processName} (pid ${d.pid})`
    : `pid ${d.pid}`;
}

function renderDumpSelector(
  trace: Trace,
  allDumps: readonly queries.HeapDump[],
  activeDump: DumpRouteRef | undefined,
): m.Children {
  if (allDumps.length <= 1) return;

  const activeDumpFull = allDumps.find(
    (d) => d.upid === activeDump?.upid && d.ts === activeDump?.ts,
  );

  return m(
    '.pf-hde-dump-selector',
    m('span.pf-hde-dump-selector__label', 'Heap dump:'),
    m(
      PopupMenu,
      {
        trigger: m(Button, {
          label: activeDumpFull
            ? processLabel(activeDumpFull)
            : 'Select a heap dump',
          icon: 'memory',
          rightIcon: 'arrow_drop_down',
          variant: ButtonVariant.Outlined,
        }),
      },
      allDumps.map((d) => {
        const offset = Time.diff(d.ts, trace.traceInfo.start);
        return m(MenuItem, {
          label: `${processLabel(d)} — ${formatDuration(trace, offset)}`,
          active: d === activeDumpFull,
          onclick: () =>
            trace.navigate(
              generateNavLink({
                tab: 'overview',
                dump: {upid: d.upid, ts: d.ts},
              }),
            ),
        });
      }),
    ),
  );
}
