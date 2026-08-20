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
import {z} from 'zod';

import {assertUnreachable, ensureExists} from '../base/assert';
import {Monitor} from '../base/monitor';
import type {Trace} from '../public/trace';
import type {Row, SqlValue} from '../trace_processor/query_result';
import {Button} from '../widgets/button';
import {EmptyState} from '../widgets/empty_state';
import {
  displaySize,
  Flamegraph,
  FLAMEGRAPH_STATE_SCHEMA,
} from '../widgets/flamegraph';
import type {FlamegraphState} from '../widgets/flamegraph';
import {ResizeHandle} from '../widgets/resize_handle';
import {Switch} from '../widgets/switch';
import {DataGrid} from './widgets/datagrid/datagrid';
import type {ColumnSchema} from './widgets/datagrid/datagrid_schema';
import {InMemoryDataSource} from './widgets/datagrid/in_memory_data_source';
import type {Column, Filter} from './widgets/datagrid/model';
import {FlamegraphPanel} from './flamegraph_panel';
import type {QueryFlamegraphDependency} from './query_flamegraph';
import type {QueryFlamegraphMetric} from './query_flamegraph';

// Drag floor fallback until the grid header has been measured.
const MIN_GRID_HEIGHT = 64;
const DEFAULT_GRID_HEIGHT = 300;

const SORT_DIRECTION_SCHEMA = z.enum(['ASC', 'DESC']);
const AGGREGATE_SCHEMA = z.enum([
  'ANY',
  'SUM',
  'AVG',
  'MIN',
  'MAX',
  'COUNT_DISTINCT',
  'P25',
  'P50',
  'P75',
  'P90',
  'P95',
  'P99',
]);

const COLLECTION_COLUMN_SCHEMA = z.object({
  id: z.string(),
  field: z.string(),
  sort: SORT_DIRECTION_SCHEMA.optional(),
  aggregate: AGGREGATE_SCHEMA.optional(),
});

// Persisted filter values only ever originate from grid UI input over
// consumer-provided rows, so plain JSON scalars suffice and keep the store
// serializable.
const FILTER_VALUE_SCHEMA = z.union([z.string(), z.number(), z.null()]);

const COLLECTION_FILTER_SCHEMA = z.union([
  z.object({
    field: z.string(),
    op: z.enum(['=', '!=', '<', '<=', '>', '>=', 'glob', 'not glob']),
    value: FILTER_VALUE_SCHEMA,
  }),
  z.object({
    field: z.string(),
    op: z.enum(['in', 'not in']),
    value: z.array(FILTER_VALUE_SCHEMA),
  }),
  z.object({
    field: z.string(),
    op: z.enum(['is null', 'is not null']),
  }),
]);

// Persisted state of a FlamegraphCollection: the shared flamegraph state
// (filters, view, selected metric), the merge toggle, and the grid's
// controlled columns/filters.
export const FLAMEGRAPH_COLLECTION_STATE_SCHEMA = z.object({
  flamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
  merge: z.boolean().default(true),
  columns: z.array(COLLECTION_COLUMN_SCHEMA).optional(),
  filters: z.array(COLLECTION_FILTER_SCHEMA).default([]),
});

export type FlamegraphCollectionState = z.infer<
  typeof FLAMEGRAPH_COLLECTION_STATE_SCHEMA
>;
type CollectionFilter = z.infer<typeof COLLECTION_FILTER_SCHEMA>;

// One grid column over the collection's rows.
export interface FlamegraphCollectionColumn {
  // The field in each row holding this column's value.
  readonly field: string;
  readonly title: string;
  // 'id' renders as a clickable entry name (and enables the distinct-value
  // filter picker), 'numeric' as a displaySize()-formatted quantity,
  // 'categorical' as plain filterable text.
  readonly kind: 'id' | 'numeric' | 'categorical';
  // Unit for numeric cells ('ns', 'B', '' for counts).
  readonly unit?: string;
}

export interface FlamegraphCollectionAttrs {
  readonly trace: Trace;

  // One row per entry, all rows carrying the same fields. Fields beyond the
  // declared columns are allowed (e.g. a key field entryKey reads). Watched
  // by identity: pass a new array to reload.
  readonly rows: ReadonlyArray<Row>;
  readonly columns: ReadonlyArray<FlamegraphCollectionColumn>;

  // Stable identity of an entry, extracted from its row.
  readonly entryKey: (row: Row) => string;

  // Metrics summing the given entries into one flamegraph. Called with all
  // working keys when merging, and with a single key per entry when
  // stepping. Returning [] means "nothing to show" for that key set. Metric
  // identities (id ?? name) must be stable across key sets so the selected
  // metric survives filtering and stepping.
  readonly metricsForKeys: (
    keys: ReadonlyArray<string>,
  ) => QueryFlamegraphMetric[];

  // Plural noun for UI strings ('profiles', 'processes', 'contexts').
  readonly entityName: string;

  // Title rendered above the flamegraph in step mode. Defaults to the key.
  readonly renderEntryTitle?: (key: string, row: Row) => m.Children;

  readonly state: FlamegraphCollectionState;
  readonly onStateChange: (state: FlamegraphCollectionState) => void;

  // Forwarded to the underlying FlamegraphPanel (see its docs); must be
  // reference-stable across renders.
  readonly dependencies?: ReadonlyArray<QueryFlamegraphDependency>;

  readonly initialGridHeightPx?: number;
  readonly className?: string;
}

// Builds the DataGrid schema for the collection's columns. `resolveKey`
// maps a rendered grid row back to its entry key: the id column may display
// a label that differs from the key (a thread name for key "utid=12"), and
// DataGrid hands cell renderers only the visible columns' values. Rows that
// cannot be resolved (the aggregate-totals row, or rows indistinguishable
// across every visible column) yield undefined and render inert.
// `currentKey` is the entry shown in step mode (highlighted in the grid);
// `onJump` navigates the flamegraph to a clicked entry.
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
      case 'categorical':
        schema[c.field] = {title: c.title, columnType: 'text'};
        break;
    }
  }
  return schema;
}

// Builds a resolver mapping a grid row, as handed to cell renderers (the
// visible columns' values only, keyed by column id), back to its entry key.
// The full data rows are fingerprinted over the visible columns; a
// fingerprint claimed by more than one distinct key resolves to undefined,
// as does a row missing any visible column (the aggregate-totals row) --
// such cells render inert rather than guessing an entry.
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

// The step-mode flamegraph title when the consumer supplies no
// renderEntryTitle: the entry's display label (the id column's value),
// falling back to the key for rows without one.
function defaultEntryTitle(
  attrs: FlamegraphCollectionAttrs,
  entry: {key: string; row: Row},
): string {
  const idField = attrs.columns.find((c) => c.kind === 'id')?.field;
  const label = idField === undefined ? undefined : entry.row[idField];
  if (label === undefined || label === null || label === '') {
    return entry.key;
  }
  return String(label);
}

// Decides whether a keydown event should step the collection: only when the
// component is visible (area-selection tabs stay mounted under a closed
// Gate), stepping applies (merge off, >1 entry), no modifier is held and the
// user is not typing into a form control.
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
  const target = e.target instanceof HTMLElement ? e.target : undefined;
  if (target !== undefined) {
    const tag = target.tagName;
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') {
      return false;
    }
    if (target.isContentEditable) return false;
  }
  return true;
}

// The grid's Filter values can, in principle, carry SqlValues the persisted
// state cannot (bigint, blobs). Coerce to JSON scalars; blobs cannot
// meaningfully round-trip through the store, so drop those filters.
function sanitizeFilters(filters: readonly Filter[]): CollectionFilter[] {
  const scalar = (v: SqlValue): string | number | null | undefined => {
    if (v === null || typeof v === 'string' || typeof v === 'number') return v;
    if (typeof v === 'bigint') return Number(v);
    return undefined;
  };
  const out: CollectionFilter[] = [];
  for (const f of filters) {
    switch (f.op) {
      case 'is null':
      case 'is not null':
        out.push({field: f.field, op: f.op});
        break;
      case 'in':
      case 'not in': {
        const values = f.value.map(scalar);
        if (!values.includes(undefined)) {
          out.push({
            field: f.field,
            op: f.op,
            value: values as Array<string | number | null>,
          });
        }
        break;
      }
      case '=':
      case '!=':
      case '<':
      case '<=':
      case '>':
      case '>=':
      case 'glob':
      case 'not glob': {
        const value = scalar(f.value);
        if (value !== undefined) {
          out.push({field: f.field, op: f.op, value});
        }
        break;
      }
      default:
        assertUnreachable(f);
    }
  }
  return out;
}

interface CollectionEntry {
  readonly key: string;
  readonly row: Row;
  readonly metrics: QueryFlamegraphMetric[];
}

// A filterable grid of entries over a flamegraph: the grid's filters select
// the working set of entries; merge on sums them into one flamegraph, merge
// off steps through them one at a time in the grid's order. The entries and
// their metrics are supplied by the caller, so any flamegraph source (pprof
// archives, stack samples, heap profiles, ...) can be viewed this way.
export class FlamegraphCollection implements m.ClassComponent<FlamegraphCollectionAttrs> {
  private attrs?: FlamegraphCollectionAttrs;

  // Long-lived data sources, rebuilt only when the rows change identity.
  // `gridSource` backs the DataGrid (passing raw rows would make it build a
  // new source every render); `querySource` computes the working set with
  // field-based aliases, decoupled from the grid's id-based model.
  private gridSource?: InMemoryDataSource;
  private querySource?: InMemoryDataSource;
  private lastRows?: ReadonlyArray<Row>;

  // Rebuilt when the working set changes (see rebuild()). Metric arrays must
  // stay reference-stable across renders or the flamegraph refetches.
  private mergedMetrics?: QueryFlamegraphMetric[];
  private mergedCount = 0;
  private perEntry?: ReadonlyArray<CollectionEntry>;
  private entryIndex = 0;
  // Entry to show once the next rebuild has computed the working set (set by
  // jumpToEntry when leaving merge mode).
  private pendingJumpKey?: string;
  // One-entry cache keeping the shown entry's flamegraph state stable when
  // it has to diverge from the master (see entryState()).
  private shownState?: {
    master?: FlamegraphState;
    key: string;
    state: FlamegraphState;
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

  oninit({attrs}: m.CVnode<FlamegraphCollectionAttrs>): void {
    this.gridHeight = attrs.initialGridHeightPx ?? DEFAULT_GRID_HEIGHT;
  }

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
      {className: attrs.className},
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

  // The grid collapses down to its chrome (toolbar + column headers); only
  // body rows shrink away.
  private minGridHeight(): number {
    const row = this.gridRowEl;
    const header = row?.querySelector('.pf-grid__header');
    if (!row || !header) return MIN_GRID_HEIGHT;
    return (
      header.getBoundingClientRect().bottom - row.getBoundingClientRect().top
    );
  }

  // Memoizes buildEntryKeyResolver on the rows' identity and the visible
  // column set.
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
      // Pivoting would regroup only the grid: the flamegraph working set is
      // computed from the flat rows, so the two views would silently
      // desync (and grouped rows carry no entry keys to jump to).
      disablePivotControls: true,
      data: ensureExists(this.gridSource),
      columns: this.columns(attrs),
      onColumnsChanged: (columns: readonly Column[]) =>
        attrs.onStateChange({...attrs.state, columns: columns.slice()}),
      filters: this.filters(attrs),
      onFiltersChanged: (f: readonly Filter[]) =>
        attrs.onStateChange({...attrs.state, filters: sanitizeFilters(f)}),
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
        m(
          '.pf-flamegraph-collection__summary',
          `${attrs.state.merge ? 'Merging' : 'Showing'} ${count} of ` +
            `${attrs.rows.length} ${attrs.entityName}`,
        ),
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

  private renderMergedFlame(attrs: FlamegraphCollectionAttrs): m.Children {
    const metrics = this.mergedMetrics;
    if (metrics === undefined) {
      return this.renderEmpty(attrs);
    }
    return m(FlamegraphPanel, {
      trace: attrs.trace,
      metrics,
      state: Flamegraph.updateState(attrs.state.flamegraphState, metrics),
      onStateChange: (s) =>
        attrs.onStateChange({...attrs.state, flamegraphState: s}),
      dependencies: attrs.dependencies,
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
    return m(
      '.pf-flamegraph-collection__flame-single',
      m(
        '.pf-flamegraph-collection__flame-cell-title',
        {title: entry.key},
        attrs.renderEntryTitle?.(entry.key, entry.row) ??
          defaultEntryTitle(attrs, entry),
      ),
      m(
        '.pf-flamegraph-collection__flame-cell-body',
        m(FlamegraphPanel, {
          trace: attrs.trace,
          metrics: entry.metrics,
          state: this.entryState(attrs, entry),
          onStateChange: (s) =>
            attrs.onStateChange({...attrs.state, flamegraphState: s}),
          dependencies: attrs.dependencies,
        }),
      ),
    );
  }

  private renderEmpty(attrs: FlamegraphCollectionAttrs): m.Children {
    return m(EmptyState, {
      icon: 'filter_alt',
      title: `No ${attrs.entityName} match`,
      detail: `Adjust the filters to select ${attrs.entityName}.`,
    });
  }

  // The shown entry's flamegraph state: the shared master state, with the
  // selected metric swapped out if this entry lacks it. Cached so the state
  // object is reference-stable across renders.
  private entryState(
    attrs: FlamegraphCollectionAttrs,
    entry: CollectionEntry,
  ): FlamegraphState {
    const master = attrs.state.flamegraphState;
    const cached = this.shownState;
    if (
      cached !== undefined &&
      cached.master === master &&
      cached.key === entry.key
    ) {
      return cached.state;
    }
    const state = Flamegraph.updateState(master, entry.metrics);
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

  // The filtered entries driving the flamegraph, in the grid's sort order.
  // Evaluated by the same DataGrid machinery that renders the grid. Projects
  // every field of the (homogeneous) rows — not just the declared columns —
  // so entryKey sees whole rows even when the key is not a grid column.
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

  // Clicking an entry in the grid shows its flamegraph, leaving merge mode
  // if needed (the Monitor-triggered rebuild consumes pendingJumpKey).
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

  // Left/right arrows step between entries in the un-merged view, unless a
  // text field is focused or this collection is hidden.
  private readonly onKeyDown = (e: KeyboardEvent): void => {
    const merge = this.attrs?.state.merge ?? true;
    const count = this.perEntry?.length ?? 0;
    if (!shouldHandleStepKey(e, this.rootEl, merge, count)) return;
    this.stepEntry(e.key === 'ArrowLeft' ? -1 : 1);
    e.preventDefault();
  };

  // Merge on: one flamegraph over the whole working set. Merge off: one
  // metric set per entry.
  private rebuild(attrs: FlamegraphCollectionAttrs): void {
    const rows = this.workingRows(attrs);

    if (attrs.state.merge) {
      this.perEntry = undefined;
      const keys = rows.map((row) => attrs.entryKey(row));
      const metrics = attrs.metricsForKeys(keys);
      this.mergedMetrics = metrics.length > 0 ? metrics : undefined;
      this.mergedCount = rows.length;
      if (this.mergedMetrics !== undefined) {
        // Reconcile the persisted state against the new metric set once, so
        // renders can reuse it by reference.
        const reconciled = Flamegraph.updateState(
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
