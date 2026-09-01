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

import './flamegraph_collection.scss';
import m from 'mithril';

import {ensureExists} from '../base/assert';
import {elementIsEditable} from '../base/dom_utils';
import {Monitor} from '../base/monitor';
import type {Trace} from '../public/trace';
import type {Row, SqlValue} from '../trace_processor/query_result';
import {Button} from '../widgets/button';
import {EmptyState} from '../widgets/empty_state';
import {displaySize, updateTreeExplorerState} from '../widgets/tree_explorer';
import type {TreeExplorerState} from '../widgets/tree_explorer';
import {ResizeHandle} from '../widgets/resize_handle';
import {Switch} from '../widgets/switch';
import {DataGrid} from './widgets/datagrid/datagrid';
import type {ColumnSchema} from './widgets/datagrid/datagrid_schema';
import {InMemoryDataSource} from './widgets/datagrid/in_memory_data_source';
import type {Column, Filter} from './widgets/datagrid/model';
import {TreeExplorerPanel} from './tree_explorer_panel';
import type {TreeExplorerQueryMetric} from './tree_explorer_fetcher';

// Fallback until the grid header has been measured.
const MIN_GRID_HEIGHT = 64;
const DEFAULT_GRID_HEIGHT = 300;

// Held by the caller for the page's lifetime. Grid filters and columns are
// view state here, not session state, so nothing is serialized.
export interface FlamegraphCollectionState {
  // Shared across entries, so stepping compares like with like.
  readonly flamegraphState?: TreeExplorerState;
  readonly merge: boolean;
  readonly columns?: readonly Column[];
  readonly filters: readonly Filter[];
}

export const DEFAULT_FLAMEGRAPH_COLLECTION_STATE: FlamegraphCollectionState = {
  merge: true,
  filters: [],
};

export interface FlamegraphCollectionColumn {
  readonly field: string;
  readonly title: string;
  // 'id' renders a clickable entry name, 'numeric' a formatted quantity.
  readonly kind: 'id' | 'numeric';
  // 'ns', 'B', or '' for counts.
  readonly unit?: string;
}

export interface FlamegraphCollectionAttrs {
  readonly trace: Trace;

  // One row per entry; extra fields are fine (e.g. one entryKey reads).
  // Watched by identity: pass a new array to reload.
  readonly rows: ReadonlyArray<Row>;
  readonly columns: ReadonlyArray<FlamegraphCollectionColumn>;

  // Stable identity of an entry.
  readonly entryKey: (row: Row) => string;

  // Metrics summing the given entries; [] means nothing to show. Metric ids
  // must be stable across key sets, so the selected measure survives.
  readonly metricsForKeys: (
    keys: ReadonlyArray<string>,
  ) => TreeExplorerQueryMetric[];

  // Plural noun for UI strings, e.g. 'profiles'.
  readonly entityName: string;

  // Title rendered above the flamegraph in step mode. Defaults to the key.
  readonly renderEntryTitle?: (key: string, row: Row) => m.Children;

  readonly state: FlamegraphCollectionState;
  readonly onStateChange: (state: FlamegraphCollectionState) => void;
}

// `resolveKey` recovers a rendered row's entry key: the id column may show a
// label that isn't the key, and DataGrid passes renderers only the visible
// columns. Unresolvable rows render inert.
export function buildCollectionGridSchema(
  columns: ReadonlyArray<FlamegraphCollectionColumn>,
  resolveKey: (row: Row) => string | undefined,
  currentKey: string | undefined,
  onJump: (key: string) => void,
): ColumnSchema {
  const schema: ColumnSchema = {};
  for (const c of columns) {
    switch (c.kind) {
      case 'id':
        schema[c.field] = {
          title: c.title,
          columnType: 'identifier',
          cellRenderer: (value: SqlValue, row: Row) => {
            const key = resolveKey(row);
            if (key === undefined) {
              return String(value ?? '');
            }
            return m(
              'span.pf-flamegraph-collection__entry' +
                (key === currentKey
                  ? '.pf-flamegraph-collection__entry--current'
                  : ''),
              {onclick: () => onJump(key)},
              String(value),
            );
          },
        };
        break;
      case 'numeric':
        schema[c.field] = {
          title: c.title,
          columnType: 'quantitative',
          cellRenderer: (value: SqlValue) =>
            typeof value === 'number' ? displaySize(value, c.unit ?? '') : '',
        };
        break;
    }
  }
  return schema;
}

// Recovers a renderer row's key by fingerprinting the full rows over the
// visible columns. Ambiguous or incomplete rows yield undefined.
export function buildEntryKeyResolver(
  rows: ReadonlyArray<Row>,
  gridColumns: ReadonlyArray<Column>,
  entryKey: (row: Row) => string,
): (row: Row) => string | undefined {
  const byFingerprint = new Map<string, string | null>();
  for (const row of rows) {
    const fp = JSON.stringify(gridColumns.map((c) => String(row[c.field])));
    const key = entryKey(row);
    const existing = byFingerprint.get(fp);
    if (existing === undefined) {
      byFingerprint.set(fp, key);
    } else if (existing !== key) {
      byFingerprint.set(fp, null);
    }
  }
  return (row: Row) => {
    if (gridColumns.some((c) => !(c.id in row))) {
      return undefined;
    }
    const fp = JSON.stringify(gridColumns.map((c) => String(row[c.id])));
    return byFingerprint.get(fp) ?? undefined;
  };
}

// A page navigated away from stays mounted behind a closed Gate, so a hidden
// collection must ignore the keys.
export function shouldHandleStepKey(
  e: KeyboardEvent,
  rootEl: HTMLElement | undefined,
  merge: boolean,
  entryCount: number,
): boolean {
  if (merge || entryCount <= 1) return false;
  if (rootEl === undefined || rootEl.offsetParent === null) return false;
  if (e.ctrlKey || e.metaKey || e.altKey) return false;
  if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') return false;
  // Checkboxes are exempt (the Merge switch keeps focus after a click); a
  // focused <select> uses arrows natively.
  if (elementIsEditable(e.target)) return false;
  return !(e.target instanceof HTMLElement && e.target.tagName === 'SELECT');
}

interface CollectionEntry {
  readonly key: string;
  readonly row: Row;
  readonly metrics: TreeExplorerQueryMetric[];
}

// A filterable grid of entries over a flamegraph: the grid's filters pick the
// working set, which merge mode sums and step mode walks one at a time.
// Entries and their metrics come from the caller.
export class FlamegraphCollection implements m.ClassComponent<FlamegraphCollectionAttrs> {
  private attrs?: FlamegraphCollectionAttrs;

  // Two sources: the DataGrid needs a long-lived one (raw rows would rebuild
  // it every render), and the working set queries by field alias, not by the
  // grid's column ids.
  private gridSource?: InMemoryDataSource;
  private querySource?: InMemoryDataSource;
  private lastRows?: ReadonlyArray<Row>;

  // Metric arrays must stay reference-stable or the flamegraph refetches.
  private mergedMetrics?: TreeExplorerQueryMetric[];
  private mergedCount = 0;
  private perEntry?: ReadonlyArray<CollectionEntry>;
  private entryIndex = 0;
  // Set by jumpToEntry when leaving merge mode; consumed by the next rebuild.
  private pendingJumpKey?: string;
  // Keeps the shown entry's state reference-stable (see entryState()).
  private shownState?: {
    master?: TreeExplorerState;
    key: string;
    state: TreeExplorerState;
  };
  private readonly monitor = new Monitor([
    () => this.attrs?.rows,
    () => this.attrs?.columns,
    () => this.attrs?.state.filters,
    () => this.attrs?.state.merge,
    // Sort only affects the un-merged view (step order); ignore it merged.
    () => (this.attrs?.state.merge === false ? this.sortKey() : undefined),
  ]);

  private gridHeight = DEFAULT_GRID_HEIGHT;
  private gridRowEl?: HTMLElement;
  private rootEl?: HTMLElement;

  oncreate({dom}: m.CVnodeDOM<FlamegraphCollectionAttrs>): void {
    this.rootEl = dom as HTMLElement;
    window.addEventListener('keydown', this.onKeyDown);
  }

  onremove(): void {
    window.removeEventListener('keydown', this.onKeyDown);
  }

  view({attrs}: m.CVnode<FlamegraphCollectionAttrs>): m.Children {
    this.attrs = attrs;
    this.monitor.ifStateChanged(() => this.rebuild(attrs));

    return m(
      '.pf-flamegraph-collection',
      m(
        '.pf-flamegraph-collection__row',
        {
          style: {height: `${this.gridHeight}px`},
          oncreate: (v: m.VnodeDOM) => {
            this.gridRowEl = v.dom as HTMLElement;
          },
        },
        this.renderGrid(attrs),
      ),
      m(ResizeHandle, {
        onResize: (deltaPx: number) => {
          this.gridHeight = Math.max(
            this.minGridHeight(),
            this.gridHeight + deltaPx,
          );
          if (this.gridRowEl) {
            this.gridRowEl.style.height = `${this.gridHeight}px`;
          }
        },
        onResizeEnd: () => m.redraw(),
      }),
      m(
        '.pf-flamegraph-collection__row.pf-flamegraph-collection__row--grow',
        this.renderFlamegraph(attrs),
      ),
    );
  }

  // The grid collapses to its chrome; only body rows shrink away.
  private minGridHeight(): number {
    const row = this.gridRowEl;
    const header = row?.querySelector('.pf-grid__header');
    if (!row || !header) return MIN_GRID_HEIGHT;
    return (
      header.getBoundingClientRect().bottom - row.getBoundingClientRect().top
    );
  }

  private keyResolverCache?: {
    rows: ReadonlyArray<Row>;
    columnsKey: string;
    resolve: (row: Row) => string | undefined;
  };

  private keyResolver(
    attrs: FlamegraphCollectionAttrs,
  ): (row: Row) => string | undefined {
    const columns = this.columns(attrs);
    const columnsKey = JSON.stringify(columns.map((c) => [c.id, c.field]));
    const cache = this.keyResolverCache;
    if (
      cache !== undefined &&
      cache.rows === attrs.rows &&
      cache.columnsKey === columnsKey
    ) {
      return cache.resolve;
    }
    const resolve = buildEntryKeyResolver(attrs.rows, columns, attrs.entryKey);
    this.keyResolverCache = {rows: attrs.rows, columnsKey, resolve};
    return resolve;
  }

  private renderGrid(attrs: FlamegraphCollectionAttrs): m.Children {
    this.ensureSources(attrs);
    const current =
      attrs.state.merge === false
        ? this.perEntry?.[this.entryIndex]?.key
        : undefined;
    return m(DataGrid, {
      className: 'pf-flamegraph-collection__grid',
      fillHeight: true,
      schema: buildCollectionGridSchema(
        attrs.columns,
        this.keyResolver(attrs),
        current,
        (key) => this.jumpToEntry(attrs, key),
      ),
      // Pivoting would regroup the grid only, silently desyncing it from the
      // flamegraph, which reads the flat rows.
      disablePivotControls: true,
      data: ensureExists(this.gridSource),
      columns: this.columns(attrs),
      onColumnsChanged: (columns: readonly Column[]) =>
        attrs.onStateChange({...attrs.state, columns: columns.slice()}),
      filters: this.filters(attrs),
      onFiltersChanged: (f: readonly Filter[]) =>
        attrs.onStateChange({...attrs.state, filters: f.slice()}),
    });
  }

  private renderFlamegraph(attrs: FlamegraphCollectionAttrs): m.Children {
    const count = attrs.state.merge
      ? this.mergedCount
      : (this.perEntry?.length ?? 0);
    const idx = this.clampIndex(this.perEntry?.length ?? 0);
    const nav =
      !attrs.state.merge && count > 0
        ? m(
            '.pf-flamegraph-collection__flame-nav',
            m(Button, {
              icon: 'chevron_left',
              compact: true,
              disabled: idx <= 0,
              title: `Previous ${attrs.entityName}`,
              onclick: () => this.stepEntry(-1),
            }),
            m('.pf-flamegraph-collection__flame-pos', `${idx + 1} / ${count}`),
            m(Button, {
              icon: 'chevron_right',
              compact: true,
              disabled: idx >= count - 1,
              title: `Next ${attrs.entityName}`,
              onclick: () => this.stepEntry(1),
            }),
          )
        : null;
    return m(
      '.pf-flamegraph-collection__flame',
      m(
        '.pf-flamegraph-collection__flame-head',
        m(Switch, {
          label: 'Merge',
          checked: attrs.state.merge,
          onchange: () =>
            attrs.onStateChange({...attrs.state, merge: !attrs.state.merge}),
        }),
        this.renderHeadLabel(attrs, idx, count),
        nav,
      ),
      m(
        '.pf-flamegraph-collection__flame-body',
        attrs.state.merge
          ? this.renderMergedFlame(attrs)
          : this.renderEntryFlame(attrs, idx),
      ),
    );
  }

  // The head's middle slot: what the flamegraph is showing -- a description
  // of the set when merging, the entry's name when stepping.
  private renderHeadLabel(
    attrs: FlamegraphCollectionAttrs,
    idx: number,
    count: number,
  ): m.Children {
    const divider = m('.pf-flamegraph-collection__head-divider');
    if (attrs.state.merge) {
      return [
        divider,
        m(
          '.pf-flamegraph-collection__summary',
          `${count} of ${attrs.rows.length} ${attrs.entityName}`,
        ),
      ];
    }
    const entry = this.perEntry?.[idx];
    if (entry === undefined) return null;
    return [
      divider,
      m(
        '.pf-flamegraph-collection__flame-cell-title',
        {title: entry.key},
        attrs.renderEntryTitle?.(entry.key, entry.row) ?? entry.key,
      ),
    ];
  }

  private renderMergedFlame(attrs: FlamegraphCollectionAttrs): m.Children {
    const metrics = this.mergedMetrics;
    if (metrics === undefined) {
      return this.renderEmpty(attrs);
    }
    return m(TreeExplorerPanel, {
      trace: attrs.trace,
      metrics,
      state: updateTreeExplorerState(attrs.state.flamegraphState, metrics),
      onStateChange: (s) =>
        attrs.onStateChange({...attrs.state, flamegraphState: s}),
    });
  }

  private renderEntryFlame(
    attrs: FlamegraphCollectionAttrs,
    idx: number,
  ): m.Children {
    const entry = this.perEntry?.[idx];
    if (entry === undefined) {
      return this.renderEmpty(attrs);
    }
    return m(TreeExplorerPanel, {
      trace: attrs.trace,
      metrics: entry.metrics,
      state: this.entryState(attrs, entry),
      onStateChange: (s) =>
        attrs.onStateChange({...attrs.state, flamegraphState: s}),
    });
  }

  private renderEmpty(attrs: FlamegraphCollectionAttrs): m.Children {
    return m(EmptyState, {
      icon: 'filter_alt',
      title: `No ${attrs.entityName} match`,
      detail: `Adjust the filters to select ${attrs.entityName}.`,
    });
  }

  // The master state, with the metric swapped out if this entry lacks it.
  // Cached to stay reference-stable.
  private entryState(
    attrs: FlamegraphCollectionAttrs,
    entry: CollectionEntry,
  ): TreeExplorerState {
    const master = attrs.state.flamegraphState;
    const cached = this.shownState;
    if (
      cached !== undefined &&
      cached.master === master &&
      cached.key === entry.key
    ) {
      return cached.state;
    }
    const state = updateTreeExplorerState(master, entry.metrics);
    this.shownState = {master, key: entry.key, state};
    return state;
  }

  private filters(attrs: FlamegraphCollectionAttrs): readonly Filter[] {
    return attrs.state.filters;
  }

  private columns(attrs: FlamegraphCollectionAttrs): readonly Column[] {
    const cols = attrs.state.columns;
    if (cols !== undefined && cols.length > 0) {
      return cols;
    }
    return attrs.columns.map((c) => ({id: c.field, field: c.field}));
  }

  private sortKey(): string | undefined {
    const attrs = this.attrs;
    if (attrs === undefined) return undefined;
    const c = this.columns(attrs).find((c) => c.sort !== undefined);
    return c && `${c.field}:${c.sort}`;
  }

  private ensureSources(attrs: FlamegraphCollectionAttrs): void {
    if (this.lastRows !== attrs.rows) {
      this.lastRows = attrs.rows;
      this.gridSource = new InMemoryDataSource(attrs.rows);
      this.querySource = new InMemoryDataSource(attrs.rows);
      this.shownState = undefined;
    }
  }

  // The filtered entries, in the grid's sort order, evaluated by the same
  // DataGrid machinery. Projects every row field, so entryKey sees whole rows.
  private workingRows(attrs: FlamegraphCollectionAttrs): ReadonlyArray<Row> {
    this.ensureSources(attrs);
    const sorted = this.columns(attrs).find((c) => c.sort !== undefined);
    const fields = Object.keys(attrs.rows[0] ?? {});
    const result = ensureExists(this.querySource).useRows({
      mode: 'flat',
      columns: fields.map((f) => ({field: f, alias: f})),
      filters: this.filters(attrs),
      sort: sorted?.sort
        ? {alias: sorted.field, direction: sorted.sort}
        : undefined,
    });
    return result.rows ?? [];
  }

  private clampIndex(count: number): number {
    if (count <= 0) return 0;
    this.entryIndex = Math.min(count - 1, Math.max(0, this.entryIndex));
    return this.entryIndex;
  }

  private stepEntry(delta: number): void {
    const n = this.perEntry?.length ?? 0;
    if (n === 0) return;
    this.entryIndex = Math.min(n - 1, Math.max(0, this.entryIndex + delta));
    m.redraw();
  }

  // Leaves merge mode if needed; the rebuild that follows consumes the key.
  private jumpToEntry(attrs: FlamegraphCollectionAttrs, key: string): void {
    if (attrs.state.merge) {
      this.pendingJumpKey = key;
      attrs.onStateChange({...attrs.state, merge: false});
    } else {
      const idx = this.perEntry?.findIndex((p) => p.key === key) ?? -1;
      if (idx >= 0) {
        this.entryIndex = idx;
      }
    }
    m.redraw();
  }

  private readonly onKeyDown = (e: KeyboardEvent): void => {
    const merge = this.attrs?.state.merge ?? true;
    const count = this.perEntry?.length ?? 0;
    if (!shouldHandleStepKey(e, this.rootEl, merge, count)) return;
    this.stepEntry(e.key === 'ArrowLeft' ? -1 : 1);
    e.preventDefault();
  };

  // Merge on: one flamegraph over the working set. Off: one set per entry.
  private rebuild(attrs: FlamegraphCollectionAttrs): void {
    const rows = this.workingRows(attrs);

    if (attrs.state.merge) {
      this.perEntry = undefined;
      const keys = rows.map((row) => attrs.entryKey(row));
      const metrics = attrs.metricsForKeys(keys);
      this.mergedMetrics = metrics.length > 0 ? metrics : undefined;
      this.mergedCount = rows.length;
      if (this.mergedMetrics !== undefined) {
        // Reconcile once, so renders can reuse the result by reference.
        const reconciled = updateTreeExplorerState(
          attrs.state.flamegraphState,
          this.mergedMetrics,
        );
        if (reconciled !== attrs.state.flamegraphState) {
          attrs.onStateChange({...attrs.state, flamegraphState: reconciled});
        }
      }
      return;
    }

    this.mergedMetrics = undefined;
    const shownKey =
      this.pendingJumpKey ?? this.perEntry?.[this.entryIndex]?.key;
    this.pendingJumpKey = undefined;
    this.perEntry = rows.flatMap((row) => {
      const key = attrs.entryKey(row);
      const metrics = attrs.metricsForKeys([key]);
      return metrics.length > 0 ? [{key, row, metrics}] : [];
    });
    // Keep the shown entry selected across working-set changes.
    const keep = this.perEntry.findIndex((p) => p.key === shownKey);
    if (keep >= 0) {
      this.entryIndex = keep;
    }
  }
}
