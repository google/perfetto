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
import {NUM} from '../../trace_processor/query_result';

import {SQL_PREAMBLE} from './components';
import {flamegraphQuery} from './views/flamegraph_objects_view';
import * as queries from './queries';
import {
  type NavState,
  type NavView,
  stateToSubpage,
  subpageToState,
} from './nav_state';
import type {OverviewData} from './types';

interface FlamegraphSelection {
  readonly pathHashes: string;
  readonly isDominator: boolean;
  readonly upid: number;
  readonly ts: bigint;
}

interface FlamegraphTab extends FlamegraphSelection {
  readonly id: number;
  count: number | null;
}

interface InstanceTab {
  readonly id: number;
  readonly objId: number;
  readonly label: string;
}

const INSTANCE_LABEL_MAX = 30;

function truncateInstanceLabel(label: string): string {
  return label.length > INSTANCE_LABEL_MAX
    ? label.slice(0, INSTANCE_LABEL_MAX) + '…'
    : label;
}

// Created per onTraceLoad and replaced on the next one, so per-trace
// state disappears together. Dump switching within a trace keeps the
// session but drops per-dump caches.
export class HeapDumpExplorerSession {
  private _nav: NavState = {view: 'overview', params: {}};
  private _navigateCallback?: (subpage: string) => void;

  private _dumps: ReadonlyArray<queries.HeapDump> = [];
  private _activeDump: queries.HeapDump | null = null;

  private readonly _flamegraphTabs: FlamegraphTab[] = [];
  private _nextFlamegraphId = 0;
  private _activeFlamegraphId: number | null = null;

  private readonly _instanceTabs: InstanceTab[] = [];
  private _nextInstanceId = 0;
  private _activeInstanceId: number | null = null;

  private _overview: OverviewData | null = null;

  constructor(
    readonly trace: Trace,
    readonly engine: Engine,
  ) {}

  get dumps(): ReadonlyArray<queries.HeapDump> {
    return this._dumps;
  }

  get activeDump(): queries.HeapDump | null {
    return this._activeDump;
  }

  async loadDumps(): Promise<void> {
    this._dumps = await queries.loadDumpsList(this.engine);
    this._activeDump = this._dumps.length > 0 ? this._dumps[0] : null;
  }

  selectDump(d: queries.HeapDump): void {
    if (this._activeDump === d) return;
    this.switchToDump(d);
    if (
      this._nav.view === 'object' ||
      this._nav.view === 'flamegraph-objects'
    ) {
      this.navigate('overview');
    }
    m.redraw();
  }

  private switchToDump(d: queries.HeapDump): void {
    this._activeDump = d;
    this.resetDumpScopedState();
    void this.loadOverview();
  }

  get nav(): NavState {
    return this._nav;
  }

  setNavigateCallback(cb: ((subpage: string) => void) | undefined): void {
    this._navigateCallback = cb;
  }

  navigate(view: NavView, params: Record<string, unknown> = {}): void {
    this._nav = {view, params} as NavState;
    this._navigateCallback?.(stateToSubpage(this._nav));
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
      this.navigate(view, params);
      return;
    }
    this._activeInstanceId = null;
    this.navigate(view, params);
  };

  readonly clearNavParam = (key: string): void => {
    delete (this._nav.params as Record<string, unknown>)[key];
  };

  syncFromSubpage(subpage: string | undefined): void {
    const sub = subpage?.startsWith('/') ? subpage.slice(1) : subpage;
    // The router strips query params from `subpage`; compare path-only.
    const currentPath = stateToSubpage(this._nav).split('?')[0];
    const incomingPath = (sub ?? '').split('?')[0];
    if (incomingPath !== currentPath) {
      this._nav = subpageToState(sub);
    }
  }

  get flamegraphTabs(): ReadonlyArray<FlamegraphTab> {
    return this._flamegraphTabs;
  }

  get activeFlamegraphId(): number | null {
    return this._activeFlamegraphId;
  }

  setActiveFlamegraphTab(id: number): void {
    this._activeFlamegraphId = id;
  }

  clearActiveFlamegraphTab(): void {
    this._activeFlamegraphId = null;
  }

  openFlamegraph(sel: FlamegraphSelection): void {
    const target = this._dumps.find(
      (d) => d.upid === sel.upid && d.ts === sel.ts,
    );
    if (target && target !== this._activeDump) {
      this.switchToDump(target);
    }
    const existing = this._flamegraphTabs.find(
      (t) =>
        t.pathHashes === sel.pathHashes && t.isDominator === sel.isDominator,
    );
    if (existing) {
      this._activeFlamegraphId = existing.id;
      this.navigate('flamegraph-objects');
      return;
    }
    const tab: FlamegraphTab = {
      id: this._nextFlamegraphId++,
      count: null,
      pathHashes: sel.pathHashes,
      isDominator: sel.isDominator,
      upid: sel.upid,
      ts: sel.ts,
    };
    this._flamegraphTabs.push(tab);
    this._activeFlamegraphId = tab.id;
    this.navigate('flamegraph-objects');

    const q = flamegraphQuery(sel.pathHashes, sel.isDominator);
    this.engine
      .query(`${SQL_PREAMBLE}; SELECT COUNT(*) AS c FROM (${q})`)
      .then((r) => {
        tab.count = Number(r.firstRow({c: NUM}).c);
        m.redraw();
      })
      .catch(console.error);
  }

  closeFlamegraph(id: number): void {
    const idx = this._flamegraphTabs.findIndex((t) => t.id === id);
    if (idx === -1) return;
    this._flamegraphTabs.splice(idx, 1);
    if (this._activeFlamegraphId === id) {
      this._activeFlamegraphId = null;
      this.navigate('overview');
    }
  }

  get instanceTabs(): ReadonlyArray<InstanceTab> {
    return this._instanceTabs;
  }

  get activeInstanceId(): number | null {
    return this._activeInstanceId;
  }

  setActiveInstanceTab(id: number): void {
    this._activeInstanceId = id;
  }

  clearActiveInstanceTab(): void {
    this._activeInstanceId = null;
  }

  private openInstanceTab(objId: number, label?: string): void {
    const existing = this._instanceTabs.find((t) => t.objId === objId);
    if (existing) {
      this._activeInstanceId = existing.id;
      return;
    }
    const tab: InstanceTab = {
      id: this._nextInstanceId++,
      objId,
      label: truncateInstanceLabel(label ?? 'Instance'),
    };
    this._instanceTabs.push(tab);
    this._activeInstanceId = tab.id;
  }

  closeInstanceTab(id: number): void {
    const idx = this._instanceTabs.findIndex((t) => t.id === id);
    if (idx === -1) return;
    this._instanceTabs.splice(idx, 1);
    if (this._activeInstanceId === id) {
      this._activeInstanceId = null;
      this.navigate('overview');
    }
  }

  syncInstanceTabFromNav(): void {
    if (this._nav.view !== 'object') {
      this._activeInstanceId = null;
      return;
    }
    const {id, label} = this._nav.params;
    const existing = this._instanceTabs.find((t) => t.objId === id);
    if (existing) {
      this._activeInstanceId = existing.id;
    } else {
      this.openInstanceTab(id, label);
    }
  }

  private resetDumpScopedState(): void {
    this._overview = null;
    this._flamegraphTabs.length = 0;
    this._nextFlamegraphId = 0;
    this._activeFlamegraphId = null;
    this._instanceTabs.length = 0;
    this._nextInstanceId = 0;
    this._activeInstanceId = null;
  }

  get cachedOverview(): OverviewData | null {
    return this._overview;
  }

  // Pins the dump at fetch start; if the user switches dumps before
  // the result arrives, the result is dropped instead of briefly
  // displaying the wrong dump's overview.
  async loadOverview(): Promise<void> {
    if (this._overview !== null) return;
    const dump = this._activeDump;
    if (dump === null) return;
    try {
      const data = await queries.getOverview(this.engine, dump);
      if (this._activeDump === dump) this._overview = data;
    } catch (err) {
      console.error('Failed to load overview:', err);
    } finally {
      m.redraw();
    }
  }
}
