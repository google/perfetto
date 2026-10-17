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

import type {Engine} from '../../trace_processor/engine';
import type {Storage, Trace} from '../../public/trace';
import type {Setting} from '../../public/settings';
import {NUM} from '../../trace_processor/query_result';

import {
  flamegraphPreamble,
  flamegraphQuery,
} from './views/flamegraph_objects_view';
import * as queries from './queries';
import type {HeapDump} from './types';
import {
  type DefaultNavView,
  type NavState,
  type NavView,
  stateToPath,
  stateToSubpage,
  subpageToState,
} from './nav_state';
import type {TreeExplorerState} from '../../widgets/tree_explorer';
import type {HdeState} from './persisted_state';
import {
  METRIC_DOMINATED_OBJECT_SIZE,
  METRIC_OBJECT_SIZE,
} from './views/flamegraph_view';
import type {time} from '../../base/time';

// Identifies a flamegraph drill-down tab: the selected frames' path hashes and
// the tree they come from.
export interface FlamegraphTabId {
  readonly pathHashes: string;
  readonly isDominator: boolean;
  // Whether the frames come from the Field Flamegraph.
  readonly isField?: boolean;
}

interface FlamegraphSelection extends FlamegraphTabId {
  readonly upid: number;
  readonly ts: time;
}

// A flamegraph drill-down tab: identity plus the title's object count (null
// until fetched).
export interface FlamegraphTabView extends FlamegraphTabId {
  readonly count: number | null;
}

export function sameFlamegraphTab(
  a: FlamegraphTabId,
  b: FlamegraphTabId,
): boolean {
  return (
    a.pathHashes === b.pathHashes &&
    a.isDominator === b.isDominator &&
    (a.isField ?? false) === (b.isField ?? false)
  );
}

const INSTANCE_LABEL_MAX = 30;

function truncateInstanceLabel(label: string): string {
  return label.length > INSTANCE_LABEL_MAX
    ? label.slice(0, INSTANCE_LABEL_MAX) + '…'
    : label;
}

// Count-cache key for a flamegraph tab.
function countKey(tab: FlamegraphTabId): string {
  return `${tab.isDominator ? 'd' : 'n'}${tab.isField ? 'f' : ''}:${tab.pathHashes}`;
}

// The persistent state lives in the registered storage `storage`, which the core
// serializes into permalinks and restores before the plugin loads. The session
// is a thin controller over it: mutations are storage edits, views render from
// the storage, restoration is automatic. Non-serializable trace-derived data (the
// dumps, per-tab counts) is cached here instead.
export class HeapDumpExplorerSession {
  private _dumps: ReadonlyArray<HeapDump> = [];
  private readonly _counts = new Map<string, number>();

  // Whether the trace has HPROF field values (heap_graph_primitive). Loaded
  // with the dumps.
  hasFieldValues = false;

  // Set when the plugin auto-redirected to HDE on load; gates the
  // "default view changed" hint on the overview.
  autoNavigated = false;

  constructor(
    readonly trace: Trace,
    readonly engine: Engine,
    readonly hideDefaultChangedHint: Setting<boolean>,
    readonly defaultFlamegraph: Setting<boolean>,
    private readonly storage: Storage<HdeState>,
  ) {}

  private edit(
    fn: (draft: {-readonly [K in keyof HdeState]: HdeState[K]}) => void,
  ): void {
    const next: {-readonly [K in keyof HdeState]: HdeState[K]} = {
      ...this.storage.get(),
    };
    fn(next);
    this.storage.set(next);
  }

  get defaultView(): DefaultNavView {
    return this.defaultFlamegraph.get() ? 'flamegraph' : 'overview';
  }

  get dumps(): ReadonlyArray<HeapDump> {
    return this._dumps;
  }

  get activeDump(): HeapDump | null {
    const ref = this.storage.get().activeDump;
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
    this.hasFieldValues = await queries.hasFieldValues(this.engine);
    const ref = this.storage.get().activeDump;
    const restored =
      ref !== undefined &&
      this._dumps.some((d) => d.upid === ref.upid && d.ts === BigInt(ref.ts));
    if (restored) {
      for (const t of this.storage.get().flamegraphTabs ?? []) {
        this.loadCount(t);
      }
    } else {
      const first = this._dumps.length > 0 ? this._dumps[0] : undefined;
      this.edit((s) => {
        s.activeDump =
          first === undefined
            ? undefined
            : {upid: first.upid, ts: first.ts.toString()};
        s.nav = undefined;
        s.flamegraphTabs = undefined;
        s.instanceTabs = undefined;
        s.flamegraphPanelState = undefined;
        s.fieldFlamegraphPanelState = undefined;
        s.callstackPanelState = undefined;
      });
    }
    return restored;
  }

  selectDump(d: HeapDump): void {
    if (this.activeDump === d) return;
    this.switchToDump(d);
    const view = this.nav.view;
    if (view === 'object' || view === 'flamegraph-objects') {
      this.navigate(this.defaultView);
    }
  }

  private switchToDump(d: HeapDump): void {
    this._counts.clear();
    this.edit((s) => {
      s.activeDump = {upid: d.upid, ts: d.ts.toString()};
      s.flamegraphTabs = undefined;
      s.instanceTabs = undefined;
      s.flamegraphPanelState = undefined;
      s.fieldFlamegraphPanelState = undefined;
      s.callstackPanelState = undefined;
    });
  }

  get nav(): NavState {
    return subpageToState(this.storage.get().nav, this.defaultView);
  }

  // The current nav as a route path (no query params).
  get navPath(): string {
    if (this.storage.get().nav === undefined) {
      return '';
    }
    return stateToPath(this.nav);
  }

  navigate(view: NavView, params: Record<string, unknown> = {}): void {
    const sub = stateToSubpage({view, params} as NavState);
    this.edit((s) => {
      s.nav = sub;
    });
    this.trace.navigate(makeHref(sub));
  }

  navigateWithTabs(view: NavView, params?: Record<string, unknown>): void {
    if (view === 'object') {
      this.openInstanceTab(
        params?.id as number,
        params?.label as string | undefined,
      );
    }
    this.navigate(view, params);
  }

  clearNavParam(key: string): void {
    // A consumed nav param becomes a one-shot grid filter, so drop it from the
    // nav — from both the store and the URL. Otherwise it re-applies on the next
    // sync and clobbers the user's later manual filter edits. Query params are
    // already gone (the router strips them), but path-encoded ones (e.g.
    // objects_<class>) survive in the URL, so we must rewrite it here.
    const nav = subpageToState(this.storage.get().nav, this.defaultView);
    delete (nav.params as Record<string, unknown>)[key];
    const sub = stateToSubpage(nav);
    this.edit((s) => {
      s.nav = sub;
    });
    // Replace the current URL instead of navigating to avoid creating a new
    // history entry and creating a navigation trap.
    location.replace(makeHref(sub));
  }

  // Mirrors URL-driven nav (back/forward, address bar) into the store, on path
  // change only. The router drops query params, so only the path round-trips.
  syncFromSubpage(subpage: string | undefined): void {
    const sub = subpage?.startsWith('/') ? subpage.slice(1) : subpage;
    const incomingPath = (sub ?? '').split('?')[0];
    if (incomingPath !== this.navPath) {
      this.edit((s) => {
        s.nav = sub
          ? stateToSubpage(subpageToState(sub, this.defaultView))
          : undefined;
      });
    }
  }

  get flamegraphTabs(): ReadonlyArray<FlamegraphTabView> {
    return (this.storage.get().flamegraphTabs ?? []).map((t) => ({
      pathHashes: t.pathHashes,
      isDominator: t.isDominator,
      isField: t.isField,
      count: this._counts.get(countKey(t)) ?? null,
    }));
  }

  // The active flamegraph tab, derived from the nav (not stored), or null.
  get activeFlamegraph(): FlamegraphTabId | null {
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
      isField: nav.params.isField,
    };
  }

  openFlamegraph(sel: FlamegraphSelection): void {
    const target = this._dumps.find(
      (d) => d.upid === sel.upid && d.ts === sel.ts,
    );
    if (target && target !== this.activeDump) {
      this.switchToDump(target);
    }
    const tab = flamegraphTabId(sel);
    this.openFlamegraphTab(tab);
    this.navigate('flamegraph-objects', {...tab});
  }

  // Adds the flamegraph tab for a selection in the active dump if not open.
  private openFlamegraphTab(tab: FlamegraphTabId): void {
    if (this.activeDump === null) return;
    const tabs = this.storage.get().flamegraphTabs ?? [];
    if (tabs.some((t) => sameFlamegraphTab(t, tab))) {
      return;
    }
    this.edit((s) => {
      s.flamegraphTabs = [...(s.flamegraphTabs ?? []), flamegraphTabId(tab)];
    });
    this.loadCount(tab);
  }

  closeFlamegraph(tab: FlamegraphTabId): void {
    const active = this.activeFlamegraph;
    this.edit((s) => {
      s.flamegraphTabs = (s.flamegraphTabs ?? []).filter(
        (t) => !sameFlamegraphTab(t, tab),
      );
    });
    if (active !== null && sameFlamegraphTab(active, tab)) {
      this.navigate(this.defaultView);
    }
  }

  syncFlamegraphTabFromNav(): void {
    const active = this.activeFlamegraph;
    if (active === null) return;
    const tabs = this.storage.get().flamegraphTabs ?? [];
    if (!tabs.some((t) => sameFlamegraphTab(t, active))) {
      this.openFlamegraphTab(active);
    }
  }

  // Fetches the title's object count for one flamegraph tab. No-op if cached.
  private loadCount(tab: FlamegraphTabId): void {
    const key = countKey(tab);
    if (this._counts.has(key)) return;
    const q = flamegraphQuery(tab.pathHashes, tab.isDominator, tab.isField);
    this.engine
      .query(
        `${flamegraphPreamble(tab.isDominator, tab.isField)}; SELECT COUNT(*) AS c FROM (${q})`,
      )
      .then((r) => {
        this._counts.set(key, r.firstRow({c: NUM}).c);
      })
      .catch(console.error);
  }

  get instanceTabs(): ReadonlyArray<{objId: number; label: string}> {
    return this.storage.get().instanceTabs ?? [];
  }

  // The active object's id, derived from the nav (not stored), or null.
  get activeInstanceObjId(): number | null {
    const nav = this.nav;
    return nav.view === 'object' ? nav.params.id : null;
  }

  private openInstanceTab(objId: number, label?: string): void {
    const tabs = this.storage.get().instanceTabs ?? [];
    if (tabs.some((t) => t.objId === objId)) return;
    this.edit((s) => {
      s.instanceTabs = [
        ...(s.instanceTabs ?? []),
        {objId, label: truncateInstanceLabel(label ?? 'Instance')},
      ];
    });
  }

  closeInstanceTab(objId: number): void {
    const wasActive = this.activeInstanceObjId === objId;
    this.edit((s) => {
      s.instanceTabs = (s.instanceTabs ?? []).filter((t) => t.objId !== objId);
    });
    if (wasActive) this.navigate(this.defaultView);
  }

  syncInstanceTabFromNav(): void {
    const nav = this.nav;
    if (nav.view !== 'object') return;
    const {id, label} = nav.params;
    const tabs = this.storage.get().instanceTabs ?? [];
    if (!tabs.some((t) => t.objId === id)) this.openInstanceTab(id, label);
  }

  get flamegraphPanelState(): TreeExplorerState | undefined {
    return this.storage.get().flamegraphPanelState;
  }

  setFlamegraphPanelState(state: TreeExplorerState): void {
    this.edit((s) => {
      s.flamegraphPanelState = state;
    });
  }

  get fieldFlamegraphPanelState(): TreeExplorerState | undefined {
    return this.storage.get().fieldFlamegraphPanelState;
  }

  setFieldFlamegraphPanelState(state: TreeExplorerState): void {
    this.edit((s) => {
      s.fieldFlamegraphPanelState = state;
    });
  }

  get callstackPanelState(): TreeExplorerState | undefined {
    return this.storage.get().callstackPanelState;
  }

  setCallstackPanelState(state: TreeExplorerState): void {
    this.edit((s) => {
      s.callstackPanelState = state;
    });
  }

  // Open the flamegraph pivoted at `pathHash`. The metric matches the tree the
  // hash came from. The chip shows `<label> (this instance)` since the raw hash
  // regex is unreadable.
  openFlamegraphPivotedAt(
    pathHash: string,
    label: string,
    isDominator: boolean,
  ): void {
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
  }
}

// The tab identity alone, without isField when unset so that state stays as
// it was before field flamegraphs existed.
function flamegraphTabId(tab: FlamegraphTabId): FlamegraphTabId {
  const {pathHashes, isDominator, isField} = tab;
  return isField
    ? {pathHashes, isDominator, isField}
    : {pathHashes, isDominator};
}

function makeHref(sub: string | undefined): string {
  return `#!/heapdump${sub ? '/' + sub : ''}`;
}
