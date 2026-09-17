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
import type {Engine} from '../../trace_processor/engine';
import type {Trace} from '../../public/trace';
import type {Setting} from '../../public/settings';
import {NUM} from '../../trace_processor/query_result';

import {SQL_PREAMBLE} from './components';
import {flamegraphQuery} from './views/flamegraph_objects_view';
import * as queries from './queries';
import {
  type DefaultNavView,
  formatHeapDumpSubpage,
  type NavState,
  type NavView,
  parseHeapDumpSubpage,
  stateToPath,
  stateToSubpage,
  subpageToState,
} from './nav_state';
import type {TreeExplorerState} from '../../widgets/tree_explorer';
import {
  METRIC_DOMINATED_OBJECT_SIZE,
  METRIC_OBJECT_SIZE,
} from './views/flamegraph_view';
import type {time} from '../../base/time';

interface FlamegraphSelection {
  readonly pathHashes: string;
  readonly isDominator: boolean;
  readonly upid: number;
  readonly ts: time;
}

// A flamegraph drill-down tab: identity plus the title's object count (null
// until fetched).
export interface FlamegraphTabView {
  readonly pathHashes: string;
  readonly isDominator: boolean;
  readonly count: number | null;
}

const INSTANCE_LABEL_MAX = 30;

function truncateInstanceLabel(label: string): string {
  return label.length > INSTANCE_LABEL_MAX
    ? label.slice(0, INSTANCE_LABEL_MAX) + '…'
    : label;
}

// Count-cache key for a flamegraph tab.
function countKey(pathHashes: string, isDominator: boolean): string {
  return `${isDominator ? 'd' : 'n'}:${pathHashes}`;
}

// In-memory controller for the Heap Dump Explorer. It holds the selected dump,
// nav, open tabs, and panel state as plain fields; the page renders from these
// getters and drives changes through the methods below. It has no persistence
// of its own — the shared-link store is mounted by the plugin (index.ts) but is
// not used here.
export class HeapDumpExplorerSession {
  private _navigateCallback?: (subpage: string, replace?: boolean) => void;

  private _dumps: ReadonlyArray<queries.HeapDump> = [];
  private readonly _counts = new Map<string, number>();

  // HDE state, held in memory.
  private _activeDump: queries.HeapDump | null = null;
  // The active nav, as a stateToSubpage subpage string (or undefined).
  private _nav: string | undefined = undefined;
  private _flamegraphTabs: {pathHashes: string; isDominator: boolean}[] = [];
  private _instanceTabs: {objId: number; label: string}[] = [];
  private _flamegraphPanelState: TreeExplorerState | undefined = undefined;
  private _callstackPanelState: TreeExplorerState | undefined = undefined;

  // Set when the plugin auto-redirected to HDE on load; gates the
  // "default view changed" hint on the overview.
  autoNavigated = false;

  constructor(
    readonly trace: Trace,
    readonly engine: Engine,
    readonly hideDefaultChangedHint: Setting<boolean>,
    readonly defaultFlamegraph: Setting<boolean>,
  ) {}

  get defaultView(): DefaultNavView {
    return this.defaultFlamegraph.get() ? 'flamegraph' : 'overview';
  }

  get dumps(): ReadonlyArray<queries.HeapDump> {
    return this._dumps;
  }

  get activeDump(): queries.HeapDump | null {
    return this._activeDump;
  }

  // Loads the dumps and selects the first as active. Returns false: the session
  // is in-memory only and no longer restores a shared-link state.
  async loadDumps(): Promise<boolean> {
    this._dumps = await queries.loadDumpsList(this.engine);
    this._activeDump = this._dumps.length > 0 ? this._dumps[0] : null;
    return false;
  }

  selectDump(d: queries.HeapDump): void {
    if (this.activeDump === d) return;
    this.switchToDump(d);
    const view = this.nav.view;
    if (view === 'object' || view === 'flamegraph-objects') {
      this.navigate(this.defaultView);
    } else {
      this._navigateCallback?.(this.fullSubpage);
    }
    m.redraw();
  }

  private switchToDump(d: queries.HeapDump): void {
    this._counts.clear();
    this._activeDump = d;
    this._flamegraphTabs = [];
    this._instanceTabs = [];
    this._flamegraphPanelState = undefined;
    this._callstackPanelState = undefined;
  }

  get nav(): NavState {
    return subpageToState(this._nav, this.defaultView);
  }

  // The current nav as a route path (no query params).
  get navPath(): string {
    if (this._nav === undefined) {
      return '';
    }
    return stateToPath(this.nav);
  }

  // The full subpage including the active dump prefix `<upid>-<ts>`.
  get fullSubpage(): string {
    const dump = this.activeDump;
    const navSub = this._nav ?? '';
    return formatHeapDumpSubpage(dump ?? undefined, navSub);
  }

  setNavigateCallback(
    cb: ((subpage: string, replace?: boolean) => void) | undefined,
  ): void {
    this._navigateCallback = cb;
  }

  navigate(view: NavView, params: Record<string, unknown> = {}): void {
    this._nav = stateToSubpage({view, params} as NavState);
    this._navigateCallback?.(this.fullSubpage);
    m.redraw();
  }

  readonly clearNavParam = (key: string): void => {
    // A consumed nav param becomes a one-shot grid filter, so drop it from the
    // nav — from both the state and the URL. Otherwise it re-applies on the next
    // sync and clobbers the user's later manual filter edits. Query params are
    // already gone (the router strips them), but path-encoded ones (e.g.
    // objects_<class>) survive in the URL, so we must rewrite it here.
    const nav = subpageToState(this._nav, this.defaultView);
    delete (nav.params as Record<string, unknown>)[key];
    this._nav = stateToSubpage(nav);
    this._navigateCallback?.(this.fullSubpage, true);
  };

  // Mirrors URL-driven nav (back/forward, address bar) into the state, on path
  // change only. The router drops query params, so only the path round-trips.
  syncFromSubpage(subpage: string | undefined): void {
    const {dump, navSubpage, state} = parseHeapDumpSubpage(
      subpage,
      this.defaultView,
    );
    let dumpChanged = false;
    if (dump !== undefined) {
      const target = this._dumps.find(
        (d) => d.upid === dump.upid && d.ts === dump.ts,
      );
      if (target && target !== this.activeDump) {
        this.switchToDump(target);
        dumpChanged = true;
      }
    }
    const incomingPath = navSubpage.split('?')[0];
    if (dumpChanged || incomingPath !== this.navPath) {
      this._nav = navSubpage ? stateToSubpage(state) : undefined;
    }
  }

  get flamegraphTabs(): ReadonlyArray<FlamegraphTabView> {
    return this._flamegraphTabs.map((t) => ({
      pathHashes: t.pathHashes,
      isDominator: t.isDominator,
      count: this._counts.get(countKey(t.pathHashes, t.isDominator)) ?? null,
    }));
  }

  // The active flamegraph tab, derived from the nav (not stored), or null.
  get activeFlamegraph(): {pathHashes: string; isDominator: boolean} | null {
    const nav = this.nav;
    if (
      nav.view !== 'flamegraph-objects' ||
      nav.params.pathHashes === undefined
    ) {
      return null;
    }
    return {
      pathHashes: nav.params.pathHashes,
      isDominator: nav.params.isDominator ?? false,
    };
  }

  openFlamegraph(sel: FlamegraphSelection): void {
    const target = this._dumps.find(
      (d) => d.upid === sel.upid && d.ts === sel.ts,
    );
    if (target && target !== this.activeDump) {
      this.switchToDump(target);
    }
    this.openFlamegraphTab(sel.pathHashes, sel.isDominator);
    this.navigate('flamegraph-objects', {
      pathHashes: sel.pathHashes,
      isDominator: sel.isDominator,
    });
  }

  // Adds the flamegraph tab for a selection in the active dump if not open.
  private openFlamegraphTab(pathHashes: string, isDominator: boolean): void {
    if (this.activeDump === null) return;
    if (
      this._flamegraphTabs.some(
        (t) => t.pathHashes === pathHashes && t.isDominator === isDominator,
      )
    ) {
      return;
    }
    this._flamegraphTabs = [
      ...this._flamegraphTabs,
      {pathHashes, isDominator},
    ];
    this.loadCount(pathHashes, isDominator);
  }

  closeFlamegraph(pathHashes: string, isDominator: boolean): void {
    const active = this.activeFlamegraph;
    this._flamegraphTabs = this._flamegraphTabs.filter(
      (t) => !(t.pathHashes === pathHashes && t.isDominator === isDominator),
    );
    if (
      active !== null &&
      active.pathHashes === pathHashes &&
      active.isDominator === isDominator
    ) {
      this.navigate(this.defaultView);
    }
  }

  syncFlamegraphTabFromNav(): void {
    const active = this.activeFlamegraph;
    if (active === null) return;
    if (
      !this._flamegraphTabs.some(
        (t) =>
          t.pathHashes === active.pathHashes &&
          t.isDominator === active.isDominator,
      )
    ) {
      this.openFlamegraphTab(active.pathHashes, active.isDominator);
    }
  }

  // Fetches the title's object count for one flamegraph tab. No-op if cached.
  private loadCount(pathHashes: string, isDominator: boolean): void {
    const key = countKey(pathHashes, isDominator);
    if (this._counts.has(key)) return;
    const q = flamegraphQuery(pathHashes, isDominator);
    this.engine
      .query(`${SQL_PREAMBLE}; SELECT COUNT(*) AS c FROM (${q})`)
      .then((r) => {
        this._counts.set(key, r.firstRow({c: NUM}).c);
        m.redraw();
      })
      .catch(console.error);
  }

  get instanceTabs(): ReadonlyArray<{objId: number; label: string}> {
    return this._instanceTabs;
  }

  // The active object's id, derived from the nav (not stored), or null.
  get activeInstanceObjId(): number | null {
    const nav = this.nav;
    return nav.view === 'object' ? nav.params.id : null;
  }

  private openInstanceTab(objId: number, label?: string): void {
    if (this._instanceTabs.some((t) => t.objId === objId)) return;
    this._instanceTabs = [
      ...this._instanceTabs,
      {objId, label: truncateInstanceLabel(label ?? 'Instance')},
    ];
  }

  closeInstanceTab(objId: number): void {
    const wasActive = this.activeInstanceObjId === objId;
    this._instanceTabs = this._instanceTabs.filter((t) => t.objId !== objId);
    if (wasActive) this.navigate(this.defaultView);
  }

  syncInstanceTabFromNav(): void {
    const nav = this.nav;
    if (nav.view !== 'object') return;
    const {id, label} = nav.params;
    if (!this._instanceTabs.some((t) => t.objId === id)) {
      this.openInstanceTab(id, label);
    }
  }

  get flamegraphPanelState(): TreeExplorerState | undefined {
    return this._flamegraphPanelState;
  }

  readonly setFlamegraphPanelState = (state: TreeExplorerState): void => {
    this._flamegraphPanelState = state;
  };

  get callstackPanelState(): TreeExplorerState | undefined {
    return this._callstackPanelState;
  }

  readonly setCallstackPanelState = (state: TreeExplorerState): void => {
    this._callstackPanelState = state;
  };

  // Open the flamegraph pivoted at `pathHash`. The metric matches the tree the
  // hash came from. The chip shows `<label> (this instance)` since the raw hash
  // regex is unreadable.
  readonly openFlamegraphPivotedAt = (
    pathHash: string,
    label: string,
    isDominator: boolean,
  ): void => {
    this.setFlamegraphPanelState({
      selectedMetricId: isDominator
        ? METRIC_DOMINATED_OBJECT_SIZE
        : METRIC_OBJECT_SIZE,
      addedMetricIds: [],
      displayMode: 'flamegraph',
      filters: [],
      view: {
        kind: 'PIVOT',
        pivot: `/^${pathHash}$/`,
        displayLabel: `${label} (this instance)`,
      },
    });
    this.navigate('flamegraph');
  };
}
