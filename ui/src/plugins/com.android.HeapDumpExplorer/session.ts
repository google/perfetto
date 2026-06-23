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
import type {Store} from '../../base/store';
import {NUM} from '../../trace_processor/query_result';

import {SQL_PREAMBLE} from './components';
import {flamegraphQuery} from './views/flamegraph_objects_view';
import * as queries from './queries';
import {
  type NavState,
  type NavView,
  stateToPath,
  stateToSubpage,
  subpageToState,
} from './nav_state';
import type {OverviewData, OomeData} from './types';
import type {FlamegraphState} from '../../widgets/flamegraph';
import type {HdeState} from './persisted_state';
import {
  METRIC_DOMINATED_OBJECT_SIZE,
  METRIC_OBJECT_SIZE,
} from './views/flamegraph_view';

interface FlamegraphSelection {
  readonly pathHashes: string;
  readonly isDominator: boolean;
  readonly upid: number;
  readonly ts: bigint;
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

// The persistent state lives in the mountStore'd `store`, which the core
// serializes into permalinks and restores before the plugin loads. The session
// is a thin controller over it: mutations are store edits, views render from
// the store, restoration is automatic. Non-serializable trace-derived data (the
// dumps, overview, oome, per-tab counts) is cached here instead.
export class HeapDumpExplorerSession {
  private _navigateCallback?: (subpage: string) => void;

  private _dumps: ReadonlyArray<queries.HeapDump> = [];
  private _overview: OverviewData | null = null;
  private _oomeData: OomeData | undefined = undefined;
  private _oomeDataLoading = false;
  private _oomeDataLoaded = false;
  private readonly _counts = new Map<string, number>();

  // Set when the plugin auto-redirected to HDE on load; gates the
  // "default view changed" hint on the overview.
  autoNavigated = false;

  constructor(
    readonly trace: Trace,
    readonly engine: Engine,
    readonly hideDefaultChangedHint: Setting<boolean>,
    private readonly store: Store<HdeState>,
  ) {}

  get dumps(): ReadonlyArray<queries.HeapDump> {
    return this._dumps;
  }

  get activeDump(): queries.HeapDump | null {
    const ref = this.store.state.activeDump;
    if (ref === undefined) return null;
    return (
      this._dumps.find((d) => d.upid === ref.upid && d.ts === BigInt(ref.ts)) ??
      null
    );
  }

  // Loads the dumps and reconciles them with the stored active dump. Returns
  // true if a valid permalink was restored, otherwise resets to the first dump.
  async loadDumps(): Promise<boolean> {
    this._dumps = await queries.loadDumpsList(this.engine);
    const ref = this.store.state.activeDump;
    const restored =
      ref !== undefined &&
      this._dumps.some((d) => d.upid === ref.upid && d.ts === BigInt(ref.ts));
    if (restored) {
      for (const t of this.store.state.flamegraphTabs ?? []) {
        this.loadCount(t.pathHashes, t.isDominator);
      }
    } else {
      const first = this._dumps.length > 0 ? this._dumps[0] : undefined;
      this.store.edit((s) => {
        s.activeDump =
          first === undefined
            ? undefined
            : {upid: first.upid, ts: first.ts.toString()};
        s.nav = undefined;
        s.flamegraphTabs = undefined;
        s.instanceTabs = undefined;
        s.flamegraphPanelState = undefined;
        s.callstackPanelState = undefined;
      });
    }
    return restored;
  }

  selectDump(d: queries.HeapDump): void {
    if (this.activeDump === d) return;
    this.switchToDump(d);
    const view = this.nav.view;
    if (view === 'object' || view === 'flamegraph-objects') {
      this.navigate('overview');
    }
    m.redraw();
  }

  private switchToDump(d: queries.HeapDump): void {
    this._overview = null;
    this._oomeData = undefined;
    this._oomeDataLoading = false;
    this._oomeDataLoaded = false;
    this._counts.clear();
    this.store.edit((s) => {
      s.activeDump = {upid: d.upid, ts: d.ts.toString()};
      s.flamegraphTabs = undefined;
      s.instanceTabs = undefined;
      s.flamegraphPanelState = undefined;
      s.callstackPanelState = undefined;
    });
    void this.loadOverview();
  }

  get nav(): NavState {
    return subpageToState(this.store.state.nav);
  }

  // The current nav as a route path (no query params).
  get navPath(): string {
    return stateToPath(this.nav);
  }

  setNavigateCallback(cb: ((subpage: string) => void) | undefined): void {
    this._navigateCallback = cb;
  }

  navigate(view: NavView, params: Record<string, unknown> = {}): void {
    const sub = stateToSubpage({view, params} as NavState);
    this.store.edit((s) => {
      s.nav = sub;
    });
    this._navigateCallback?.(sub);
    m.redraw();
  }

  // Arrow property: passed by reference into Mithril attrs.
  readonly navigateWithTabs = (
    view: NavView,
    params?: Record<string, unknown>,
  ): void => {
    if (view === 'object') {
      this.openInstanceTab(
        params?.id as number,
        params?.label as string | undefined,
      );
    }
    this.navigate(view, params);
  };

  readonly clearNavParam = (key: string): void => {
    // A consumed nav param (e.g. ?cls=Foo) becomes a one-shot grid filter, so
    // drop it from the nav. Otherwise it would re-apply on restore and clobber
    // the user's later manual filter edits to the same grid.
    this.store.edit((s) => {
      const nav = subpageToState(s.nav);
      delete (nav.params as Record<string, unknown>)[key];
      s.nav = stateToSubpage(nav);
    });
  };

  // Mirrors URL-driven nav (back/forward, address bar) into the store, on path
  // change only. The router drops query params, so only the path round-trips.
  syncFromSubpage(subpage: string | undefined): void {
    const sub = subpage?.startsWith('/') ? subpage.slice(1) : subpage;
    const incomingPath = (sub ?? '').split('?')[0];
    if (incomingPath !== this.navPath) {
      this.store.edit((s) => {
        s.nav = stateToSubpage(subpageToState(sub));
      });
    }
  }

  get flamegraphTabs(): ReadonlyArray<FlamegraphTabView> {
    return (this.store.state.flamegraphTabs ?? []).map((t) => ({
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
    const tabs = this.store.state.flamegraphTabs ?? [];
    if (
      tabs.some(
        (t) => t.pathHashes === pathHashes && t.isDominator === isDominator,
      )
    ) {
      return;
    }
    this.store.edit((s) => {
      s.flamegraphTabs = [
        ...(s.flamegraphTabs ?? []),
        {pathHashes, isDominator},
      ];
    });
    this.loadCount(pathHashes, isDominator);
  }

  closeFlamegraph(pathHashes: string, isDominator: boolean): void {
    const active = this.activeFlamegraph;
    this.store.edit((s) => {
      s.flamegraphTabs = (s.flamegraphTabs ?? []).filter(
        (t) => !(t.pathHashes === pathHashes && t.isDominator === isDominator),
      );
    });
    if (
      active !== null &&
      active.pathHashes === pathHashes &&
      active.isDominator === isDominator
    ) {
      this.navigate('overview');
    }
  }

  syncFlamegraphTabFromNav(): void {
    const active = this.activeFlamegraph;
    if (active === null) return;
    const tabs = this.store.state.flamegraphTabs ?? [];
    if (
      !tabs.some(
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
    return this.store.state.instanceTabs ?? [];
  }

  // The active object's id, derived from the nav (not stored), or null.
  get activeInstanceObjId(): number | null {
    const nav = this.nav;
    return nav.view === 'object' ? nav.params.id : null;
  }

  private openInstanceTab(objId: number, label?: string): void {
    const tabs = this.store.state.instanceTabs ?? [];
    if (tabs.some((t) => t.objId === objId)) return;
    this.store.edit((s) => {
      s.instanceTabs = [
        ...(s.instanceTabs ?? []),
        {objId, label: truncateInstanceLabel(label ?? 'Instance')},
      ];
    });
  }

  closeInstanceTab(objId: number): void {
    const wasActive = this.activeInstanceObjId === objId;
    this.store.edit((s) => {
      s.instanceTabs = (s.instanceTabs ?? []).filter((t) => t.objId !== objId);
    });
    if (wasActive) this.navigate('overview');
  }

  syncInstanceTabFromNav(): void {
    const nav = this.nav;
    if (nav.view !== 'object') return;
    const {id, label} = nav.params;
    const tabs = this.store.state.instanceTabs ?? [];
    if (!tabs.some((t) => t.objId === id)) this.openInstanceTab(id, label);
  }

  get flamegraphPanelState(): FlamegraphState | undefined {
    return this.store.state.flamegraphPanelState;
  }

  readonly setFlamegraphPanelState = (state: FlamegraphState): void => {
    this.store.edit((s) => {
      s.flamegraphPanelState = state;
    });
  };

  get callstackPanelState(): FlamegraphState | undefined {
    return this.store.state.callstackPanelState;
  }

  readonly setCallstackPanelState = (state: FlamegraphState): void => {
    this.store.edit((s) => {
      s.callstackPanelState = state;
    });
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
      selectedMetricName: isDominator
        ? METRIC_DOMINATED_OBJECT_SIZE
        : METRIC_OBJECT_SIZE,
      filters: [],
      view: {
        kind: 'PIVOT',
        pivot: `^${pathHash}$`,
        displayLabel: `${label} (this instance)`,
      },
    });
    this.navigate('flamegraph');
  };

  get cachedOverview(): OverviewData | null {
    return this._overview;
  }

  get cachedOomeData(): OomeData | undefined {
    return this._oomeData;
  }

  get isOomeDataLoaded(): boolean {
    return this._oomeDataLoaded;
  }

  // Pins the dump at fetch start; if the user switches dumps before the result
  // arrives, the result is dropped instead of briefly showing the wrong dump.
  async loadOverview(): Promise<void> {
    if (this._overview !== null) return;
    const dump = this.activeDump;
    if (dump === null) return;
    try {
      const data = await queries.getOverview(this.engine, dump);
      if (this.activeDump === dump) {
        this._overview = data;
      }
    } catch (err) {
      console.error('Failed to load overview:', err);
    } finally {
      m.redraw();
    }
  }

  async loadOome(): Promise<void> {
    if (this._oomeDataLoaded || this._oomeDataLoading) return;
    this._oomeDataLoading = true;
    const dump = this.activeDump;
    if (dump === null) {
      this._oomeDataLoading = false;
      return;
    }
    try {
      const oomeData = await queries.getOome(this.engine, dump);
      if (this.activeDump === dump) {
        this._oomeData = oomeData;
        this._oomeDataLoaded = true;
      }
    } catch (err) {
      console.error('Failed to load OOME:', err);
      if (this.activeDump === dump) {
        this._oomeData = undefined;
        this._oomeDataLoaded = true;
      }
    } finally {
      this._oomeDataLoading = false;
      m.redraw();
    }
  }
}
