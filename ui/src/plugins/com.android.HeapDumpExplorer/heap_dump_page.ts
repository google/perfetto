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
import {EmptyState} from '../../widgets/empty_state';
import {Tabs} from '../../widgets/tabs';
import type {TabsTab} from '../../widgets/tabs';
import type {NavState, NavView} from './nav_state';
import type {Engine} from '../../trace_processor/engine';
import type {OverviewData} from './types';
import * as queries from './queries';
import OverviewView from './views/overview_view';
import DominatorsView from './views/dominators_view';
import ObjectView from './views/object_view';
import AllObjectsView from './views/all_objects_view';
import BitmapGalleryView from './views/bitmap_gallery_view';
import ClassesView from './views/classes_view';
import StringsView from './views/strings_view';
import ArraysView from './views/arrays_view';
import FlamegraphObjectsView from './views/flamegraph_objects_view';
import FlamegraphView from './views/flamegraph_view';
import type {HeapDumpExplorerSession} from './session';

import {
  baselineDumpFilterSql,
  dispose as disposeBaseline,
  getActiveBaseline,
  getMode,
  isDiffActive,
} from './baseline/state';
import {TopBar} from './top_bar';
import ClassesDiffView from './views/diff/classes_diff_view';
import StringsDiffView from './views/diff/strings_diff_view';
import ArraysDiffView from './views/diff/arrays_diff_view';
import BitmapsDiffView from './views/diff/bitmaps_diff_view';
import DominatorsDiffView from './views/diff/dominators_diff_view';
import AllObjectsDiffView from './views/diff/all_objects_diff_view';
import ObjectDiffView from './views/diff/object_diff_view';

interface HeapDumpPageAttrs {
  readonly session: HeapDumpExplorerSession;
  readonly subpage: string | undefined;
}

const FG_KEY_PREFIX = 'fg-';
const INSTANCE_KEY_PREFIX = 'inst-';

// Overview cache keyed on (engine identity, filter SQL): in diff/baseline mode
// the page shows overviews from more than one engine (primary + pooled
// baseline), and one engine serves several dumps over the page's lifetime, so
// neither (upid, ts) nor engine identity alone is a sufficient key.
const overviewCache = new Map<string, OverviewData>();
const overviewLoadingFor = new Set<string>();

let nextEngineUid = 1;
const engineUid = new WeakMap<Engine, number>();
function engineKey(engine: Engine, filterSql: string): string {
  let id = engineUid.get(engine);
  if (id === undefined) {
    id = nextEngineUid++;
    engineUid.set(engine, id);
  }
  return `${id}:${filterSql}`;
}

// The page's last (primary | baseline | diff) tab context. A context flip
// invalidates any open object tabs (a paired diff tab is meaningless once we
// leave diff mode), so we reset them on change. `null` means "no prior
// context", which the flip check below treats as "don't clear" so a
// permalink-restored object tab survives the first render.
let lastTabContext: string | null = null;

// Drops all page-level caches. Called on every trace load (see index.ts) so
// state from a previously-loaded trace never bleeds into the next one.
export function resetCachedOverview(): void {
  overviewCache.clear();
  overviewLoadingFor.clear();
  lastTabContext = null;
}

function fgTabKey(pathHashes: string, isDominator: boolean): string {
  return `${FG_KEY_PREFIX}${isDominator ? 'd' : 'n'}:${pathHashes}`;
}

function instanceTabKey(objId: number): string {
  return `${INSTANCE_KEY_PREFIX}${objId}`;
}

function activeTabKey(session: HeapDumpExplorerSession): string {
  const tabs = session.flamegraphTabs;
  if (session.nav.view === 'flamegraph-objects' && tabs.length > 0) {
    const active = session.activeFlamegraph;
    const tab =
      (active &&
        tabs.find(
          (t) =>
            t.pathHashes === active.pathHashes &&
            t.isDominator === active.isDominator,
        )) ||
      tabs[tabs.length - 1];
    return fgTabKey(tab.pathHashes, tab.isDominator);
  }
  const objId = session.activeInstanceObjId;
  if (objId !== null) {
    return instanceTabKey(objId);
  }
  return session.nav.view;
}

// Per-tab select/close actions, looked up by key.
interface TabActions {
  select(): void;
  close?(): void;
}

function buildTabs(
  session: HeapDumpExplorerSession,
  activeDump: queries.HeapDump,
  state: NavState,
  overview: OverviewData,
  baselineOverview: OverviewData | undefined,
  baselineLoading: boolean,
): {tabs: TabsTab[]; actions: Map<string, TabActions>} {
  const {engine, trace, navigateWithTabs, clearNavParam} = session;
  const hideExplanationSetting = session.hideDefaultChangedHint;
  const hideHint = hideExplanationSetting.get();
  const actions = new Map<string, TabActions>();
  const diffActive = isDiffActive();
  const activeBaseline = getActiveBaseline();
  const baselineEngine = activeBaseline?.trace.engine;
  const tabs: TabsTab[] = [
    {
      key: 'overview',
      title: 'Overview',
      content: m(OverviewView, {
        overview,
        activeDump,
        diffActive,
        baselineOverview: diffActive ? baselineOverview : undefined,
        baselineLoading: diffActive && baselineLoading,
        navigate: navigateWithTabs,
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
        ts: Time.fromRaw(activeDump.ts),
        state: session.flamegraphPanelState,
        onStateChange: session.setFlamegraphPanelState,
        onShowObjects: (pathHashes, isDominator) =>
          session.openFlamegraph({
            pathHashes,
            isDominator,
            upid: activeDump.upid,
            ts: activeDump.ts,
          }),
        // For SAME-trace diff, leave baselineEngine unset → SQL JOIN path.
        // For CROSS-trace diff, pass the baseline engine → FlamegraphView
        // pairs in JS and writes paired rows back into the cur engine.
        baseline:
          diffActive && activeBaseline?.dump
            ? {
                upid: activeBaseline.dump.upid,
                ts: Time.fromRaw(activeBaseline.dump.ts),
              }
            : undefined,
        baselineEngine:
          diffActive && activeBaseline?.trace.disposable === true
            ? activeBaseline.trace.engine
            : undefined,
      }),
    },
    {
      key: 'classes',
      title: 'Classes',
      content:
        diffActive && baselineEngine
          ? m(ClassesDiffView, {
              currentEngine: engine,
              baselineEngine,
              navigate: navigateWithTabs,
            })
          : m(ClassesView, {
              engine,
              activeDump,
              navigate: navigateWithTabs,
              clearNavParam,
              initialRootClass:
                state.view === 'classes' ? state.params.rootClass : undefined,
            }),
    },
    {
      key: 'objects',
      title: 'Objects',
      content:
        diffActive && baselineEngine
          ? m(AllObjectsDiffView, {
              currentEngine: engine,
              baselineEngine,
              cls: state.view === 'objects' ? state.params.cls : undefined,
              navigate: navigateWithTabs,
            })
          : m(AllObjectsView, {
              engine,
              activeDump,
              navigate: navigateWithTabs,
              clearNavParam,
              initialClass:
                state.view === 'objects' ? state.params.cls : undefined,
            }),
    },
    {
      key: 'dominators',
      title: 'Dominators',
      content:
        diffActive && baselineEngine
          ? m(DominatorsDiffView, {
              currentEngine: engine,
              baselineEngine,
              navigate: navigateWithTabs,
            })
          : m(DominatorsView, {
              engine,
              activeDump,
              navigate: navigateWithTabs,
            }),
    },
    {
      key: 'bitmaps',
      title: 'Bitmaps',
      content:
        diffActive && baselineEngine
          ? m(BitmapsDiffView, {
              currentEngine: engine,
              baselineEngine,
              navigate: navigateWithTabs,
            })
          : m(BitmapGalleryView, {
              engine,
              activeDump,
              navigate: navigateWithTabs,
              clearNavParam,
              hasFieldValues: overview.hasFieldValues,
              filterKey:
                state.view === 'bitmaps' ? state.params.filterKey : undefined,
            }),
    },
    {
      key: 'strings',
      title: 'Strings',
      content:
        diffActive && baselineEngine
          ? m(StringsDiffView, {
              currentEngine: engine,
              baselineEngine,
              navigate: navigateWithTabs,
            })
          : m(StringsView, {
              engine,
              activeDump,
              navigate: navigateWithTabs,
              clearNavParam,
              initialQuery:
                state.view === 'strings' ? state.params.q : undefined,
              hasFieldValues: overview.hasFieldValues,
            }),
    },
    {
      key: 'arrays',
      title: 'Arrays',
      content:
        diffActive && baselineEngine
          ? m(ArraysDiffView, {
              currentEngine: engine,
              baselineEngine,
              navigate: navigateWithTabs,
            })
          : m(ArraysView, {
              engine,
              activeDump,
              navigate: navigateWithTabs,
              clearNavParam,
              initialArrayHash:
                state.view === 'arrays' ? state.params.arrayHash : undefined,
              hasFieldValues: overview.hasFieldValues,
            }),
    },
  ];

  // Static tab keys are view names.
  for (const tab of tabs) {
    actions.set(tab.key, {select: () => session.navigate(tab.key as NavView)});
  }

  for (const fg of session.flamegraphTabs) {
    const key = fgTabKey(fg.pathHashes, fg.isDominator);
    tabs.push({
      key,
      title:
        fg.count !== null
          ? `Flamegraph objects (${fg.count.toLocaleString()})`
          : 'Flamegraph objects',
      closeButton: true,
      content: m(FlamegraphObjectsView, {
        engine,
        navigate: navigateWithTabs,
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
      content:
        diffActive && baselineEngine
          ? m(ObjectDiffView, {
              currentEngine: engine,
              baselineEngine,
              activeDump,
              currentId: obj.currentId,
              baselineId: obj.baselineId,
              navigate: navigateWithTabs,
            })
          : m(ObjectView, {
              engine,
              activeDump,
              heaps: overview.heaps,
              navigate: navigateWithTabs,
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

export class HeapDumpPage implements m.ClassComponent<HeapDumpPageAttrs> {
  oncreate({attrs}: m.VnodeDOM<HeapDumpPageAttrs>) {
    attrs.session.setNavigateCallback((sub) => {
      window.location.hash = `!/heapdump${sub ? '/' + sub : ''}`;
    });
  }

  onremove({attrs}: m.VnodeDOM<HeapDumpPageAttrs>) {
    attrs.session.setNavigateCallback(undefined);
  }

  // Loads the overview for one (engine, filter) into the shared cache. No-op if
  // already cached or in flight. View-driven so it serves the primary dump, a
  // baseline-only view, and either side of a diff with the same code path.
  private kickOverviewLoadFor(engine: Engine | null, filterSql: string): void {
    if (!engine) return;
    const key = engineKey(engine, filterSql);
    if (overviewCache.has(key) || overviewLoadingFor.has(key)) return;
    overviewLoadingFor.add(key);
    queries
      .getOverview(engine, filterSql)
      .then((data) => {
        overviewCache.set(key, data);
      })
      .catch((err) => {
        console.error('Failed to load overview:', err);
      })
      .finally(() => {
        overviewLoadingFor.delete(key);
        m.redraw();
      });
  }

  view({attrs}: m.Vnode<HeapDumpPageAttrs>) {
    const {session, subpage} = attrs;
    session.syncFromSubpage(subpage);
    session.syncInstanceTabFromNav();
    session.syncFlamegraphTabFromNav();

    const active = session.activeDump;
    if (active === null) {
      return m(
        'div',
        {class: 'pf-hde-page'},
        m(EmptyState, {
          icon: 'memory',
          title: 'No heap graph data in this trace',
          fillHeight: true,
        }),
      );
    }

    const topBar = m(TopBar, {
      trace: session.trace,
      session,
      onDumpChanged: () => {},
    });

    const mode = getMode();
    const baseline = getActiveBaseline();
    const activeIsBaseline = mode === 'baseline' && baseline !== null;
    // Baseline-only mode points the standard (non-diff) views at the baseline
    // dump without changing the user-visible primary selection.
    queries.setDumpFilterOverride(activeIsBaseline ? baseline!.dump : null);
    queries.setActiveDumpForDiff(session.activeDump);

    const diffActive = isDiffActive();
    const tabContext = diffActive
      ? 'diff'
      : activeIsBaseline
        ? 'baseline'
        : 'primary';
    if (lastTabContext !== null && lastTabContext !== tabContext) {
      session.clearInstanceTabs();
    }
    lastTabContext = tabContext;

    const overviewEngine = activeIsBaseline
      ? baseline!.trace.engine
      : session.engine;
    // Always filter to the dump the user is actually viewing — passing
    // `undefined` would rely on dumpFilterOverride, which is null in diff mode
    // and yields '1=1' (aggregates across every dump in the engine).
    const overviewDump = activeIsBaseline ? baseline!.dump : active;
    const overviewFilter = queries.dumpFilterSql(overviewDump, 'o');
    this.kickOverviewLoadFor(overviewEngine, overviewFilter);
    const overview = overviewCache.get(
      engineKey(overviewEngine, overviewFilter),
    );

    const baselineEngine = baseline?.trace.engine ?? null;
    const baselineFilter = baselineDumpFilterSql('o');
    if (baselineEngine && mode === 'diff') {
      this.kickOverviewLoadFor(baselineEngine, baselineFilter);
    }
    const baselineCacheKey =
      baselineEngine !== null
        ? engineKey(baselineEngine, baselineFilter)
        : null;
    const baselineOverview =
      baselineCacheKey !== null
        ? overviewCache.get(baselineCacheKey)
        : undefined;
    const baselineLoading =
      baselineCacheKey !== null &&
      baselineOverview === undefined &&
      overviewLoadingFor.has(baselineCacheKey);

    if (!overview) {
      return m(
        'div',
        {class: 'pf-hde-page'},
        topBar,
        m('div', {class: 'pf-hde-loading'}, m(Spinner, {easing: true})),
      );
    }

    // Key the Tabs widget on (primary dump, baseline dump, mode) — a mode or
    // baseline change swaps the engines / filters captured by the standard
    // views at oninit, so remounting forces a re-fetch.
    const primaryKey = `${active.upid}:${active.ts}`;
    const baselineKey = baseline
      ? `${baseline.trace.id}:${baseline.dump.upid}:${baseline.dump.ts}`
      : 'none';
    const tabsKey = `${primaryKey}|${baselineKey}|${mode}`;
    const {tabs, actions} = buildTabs(
      session,
      active,
      session.nav,
      overview,
      baselineOverview,
      baselineLoading,
    );

    return m(
      'div',
      {class: 'pf-hde-page'},
      topBar,
      m(
        'main',
        {class: 'pf-hde-main'},
        m(Tabs, {
          key: tabsKey,
          tabs,
          activeTabKey: activeTabKey(session),
          onTabChange: (key: string) => actions.get(key)?.select(),
          onTabClose: (key: string) => actions.get(key)?.close?.(),
        }),
      ),
    );
  }
}

/** Re-exported convenience for index.ts so it can dispose on trace change. */
export {disposeBaseline};
