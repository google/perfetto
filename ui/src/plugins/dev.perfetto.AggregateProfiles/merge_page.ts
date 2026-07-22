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

import './merge_page.scss';
import m from 'mithril';

import type {Trace} from '../../public/trace';
import type {QueryFlamegraphMetric} from '../../components/query_flamegraph';
import {FlamegraphPanel} from '../../components/flamegraph_panel';
import {Flamegraph, displaySize, toTags} from '../../widgets/flamegraph';
import type {FlamegraphState} from '../../widgets/flamegraph';
import {Anchor} from '../../widgets/anchor';
import {Switch} from '../../widgets/switch';
import {Button, ButtonBar} from '../../widgets/button';
import {EmptyState} from '../../widgets/empty_state';
import {ResizeHandle} from '../../widgets/resize_handle';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {GridFilterChip} from '../../components/widgets/datagrid/datagrid_toolbar';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import type {
  DataSourceModel,
  DataSourceRows,
} from '../../components/widgets/datagrid/data_source';
import type {ColumnSchema} from '../../components/widgets/datagrid/datagrid_schema';
import type {Column, Filter} from '../../components/widgets/datagrid/model';
import {HistogramSvg} from '../../components/widgets/charts_svg/histogram_svg';
import {computeHistogram} from '../../components/widgets/charts/histogram_loader';
import {userFilterToRegex} from '../../widgets/flamegraph_regex';
import {NUM_NULL, STR} from '../../trace_processor/query_result';
import type {Engine} from '../../trace_processor/engine';
import type {Row, SqlValue} from '../../trace_processor/query_result';
import type {
  MergeColumn,
  MergeProfile,
  MergePageState,
  SampleType,
} from './types';

// A drawer (charts / grid) can be dragged or collapsed down to just its header;
// keeping this floor means the resize handle never disappears.
const DRAWER_HEAD_H = 26;

export interface AggregateProfilesMergePageAttrs {
  readonly trace: Trace;
  readonly profiles: ReadonlyArray<MergeProfile>;
  readonly sampleTypes: ReadonlyArray<SampleType>;
  readonly columns: ReadonlyArray<MergeColumn>;
  readonly rows: ReadonlyArray<Row>;
  readonly state: MergePageState;
  readonly onStateChange: (state: MergePageState) => void;
}

// Crossfilter explorer: a DataGrid of the profiles plus a brushable histogram
// per sample-type (sharing the grid's Filter[]), driving a merged flamegraph.
export class AggregateProfilesMergePage implements m.ClassComponent<AggregateProfilesMergePageAttrs> {
  // Merged view.
  private flamegraphMetrics?: ReadonlyArray<QueryFlamegraphMetric>;
  private flamegraphState?: FlamegraphState;
  // Un-merged view: one flamegraph per profile, state kept per scope.
  private perProfile?: ReadonlyArray<{
    scope: string;
    metrics: QueryFlamegraphMetric[];
  }>;
  private readonly profileStates = new Map<string, FlamegraphState>();
  // Master-state signature the per-profile states were last derived from; when
  // it changes they re-inherit it (see syncPerProfileStates).
  private perProfileSig?: string;
  // Un-merged: index of the profile shown (one flamegraph at a time, navigated
  // by the header arrows / left-right keys). Clamped to the filtered list.
  private profileIndex = 0;
  // Mirror of attrs.state.merge, so the key handler can ignore keys when merged.
  private merged = true;
  private lastRebuildKey = '';
  private scopeToProfile?: Map<string, MergeProfile>;
  // Resizable drawer heights (px); the flamegraph row takes the rest. During a
  // drag the height is applied straight to the DOM (chartsRowEl/gridRowEl) so the
  // heavy page isn't re-rendered on every pixel; a single redraw runs on end.
  // *Prev holds the pre-collapse height so the header chevron can restore it.
  private chartsHeight = 152;
  private gridHeight = 260;
  private chartsPrev = 152;
  private gridPrev = 260;
  private chartsRowEl?: HTMLElement;
  private gridRowEl?: HTMLElement;
  // Per-profile totals for every sample-type under the flamegraph's stack
  // filters, keyed sample-type key -> scope -> total. While set, profiles with
  // no matching stacks are excluded from the grid and navigation and the
  // sample-type columns show these totals; the user's grid filters and the
  // histograms always evaluate the raw totals, keeping brush ranges in raw
  // units. Recomputed (async) when the stack filters change; a seq guards
  // against stale results.
  private filteredValues?: Map<string, Map<string, number>>;
  private filteredSig = '';
  private filteredSeq = 0;
  private filteredVer = 0;
  private gridSource?: MergeDataSource;
  private gridSourceSig?: string;

  oncreate(): void {
    window.addEventListener('keydown', this.onKeyDown);
  }

  onremove(): void {
    window.removeEventListener('keydown', this.onKeyDown);
  }

  view({attrs}: m.CVnode<AggregateProfilesMergePageAttrs>): m.Children {
    if (this.scopeToProfile === undefined) {
      this.scopeToProfile = new Map(attrs.profiles.map((p) => [p.scope, p]));
    }
    this.merged = attrs.state.merge;
    const filters = this.filters(attrs);

    // The un-merged flamegraph follows the grid's order and membership, so a
    // sort change or new filtered totals must rebuild it; the merged view is
    // order-independent, so ignore sort there.
    const sort = activeSort(this.visibleColumns(attrs));
    const rebuildKey = JSON.stringify({
      m: attrs.state.merge,
      f: filters,
      s: attrs.state.merge ? null : sort,
      v: this.filteredVer,
    });
    if (rebuildKey !== this.lastRebuildKey) {
      this.lastRebuildKey = rebuildKey;
      this.rebuildFlamegraph(attrs);
    }
    this.updateFilteredTotals(attrs);

    return m(
      '.pf-aggregate-merge',
      this.renderDrawer({
        title: 'Distributions',
        height: this.chartsHeight,
        onEl: (el) => (this.chartsRowEl = el),
        onResize: (d) => this.resizeCharts(d),
        onToggle: () => this.toggleCharts(),
        body: this.renderCharts(attrs, filters),
      }),
      this.renderDrawer({
        title: 'Profiles',
        height: this.gridHeight,
        onEl: (el) => (this.gridRowEl = el),
        onResize: (d) => this.resizeGrid(d),
        onToggle: () => this.toggleGrid(),
        chips: this.stackTags(attrs),
        body: this.renderGrid(attrs, filters),
      }),
      m(
        '.pf-aggregate-merge__row.pf-aggregate-merge__row--grow',
        this.renderFlamegraph(attrs),
      ),
    );
  }

  // A collapsible drawer: a header (title + collapse chevron) over a body, sized
  // by `height` and followed by a resize handle that is always present — so the
  // drawer can shrink to just its header without the handle ever disappearing.
  private renderDrawer(opts: {
    title: string;
    height: number;
    onEl: (el: HTMLElement) => void;
    onResize: (deltaPx: number) => void;
    onToggle: () => void;
    chips?: ReadonlyArray<{label: string; onRemove: () => void}>;
    body: m.Children;
  }): m.Children {
    const collapsed = opts.height <= DRAWER_HEAD_H;
    return [
      m(
        '.pf-aggregate-merge__drawer',
        {
          style: {height: `${opts.height}px`},
          oncreate: (v: m.VnodeDOM) => opts.onEl(v.dom as HTMLElement),
        },
        m(
          '.pf-aggregate-merge__drawer-head',
          m('span', opts.title),
          // Same chip component the grid uses for its own filters, so the
          // flamegraph's filters read consistently next to them.
          opts.chips?.map(({label, onRemove}) =>
            m(GridFilterChip, {content: label, onRemove}),
          ),
          // Same show/hide control as the timeline's details panel, with the
          // arrow mirrored since these drawers collapse upwards.
          m(
            ButtonBar,
            {className: 'pf-aggregate-merge__drawer-buttons'},
            m(Button, {
              title: collapsed ? 'Show panel' : 'Hide panel',
              icon: collapsed ? 'keyboard_arrow_down' : 'keyboard_arrow_up',
              onclick: opts.onToggle,
            }),
          ),
        ),
        m('.pf-aggregate-merge__drawer-body', opts.body),
      ),
      m(ResizeHandle, {
        onResize: opts.onResize,
        onResizeEnd: () => m.redraw(),
      }),
    ];
  }

  private resizeCharts(deltaPx: number): void {
    this.chartsHeight = Math.max(DRAWER_HEAD_H, this.chartsHeight + deltaPx);
    if (this.chartsHeight > DRAWER_HEAD_H) this.chartsPrev = this.chartsHeight;
    if (this.chartsRowEl) {
      this.chartsRowEl.style.height = `${this.chartsHeight}px`;
    }
  }

  private resizeGrid(deltaPx: number): void {
    this.gridHeight = Math.max(DRAWER_HEAD_H, this.gridHeight + deltaPx);
    if (this.gridHeight > DRAWER_HEAD_H) this.gridPrev = this.gridHeight;
    if (this.gridRowEl) {
      this.gridRowEl.style.height = `${this.gridHeight}px`;
    }
  }

  // The chevron collapses a drawer to its header, or restores the height it had
  // before collapsing.
  private toggleCharts(): void {
    this.chartsHeight =
      this.chartsHeight > DRAWER_HEAD_H ? DRAWER_HEAD_H : this.chartsPrev;
    m.redraw();
  }

  private toggleGrid(): void {
    this.gridHeight =
      this.gridHeight > DRAWER_HEAD_H ? DRAWER_HEAD_H : this.gridPrev;
    m.redraw();
  }

  // Clamps the current profile index to the filtered list and returns it, so a
  // shrinking list can't leave us pointing past the end.
  private clampIndex(count: number): number {
    if (count <= 0) return 0;
    this.profileIndex = Math.min(count - 1, Math.max(0, this.profileIndex));
    return this.profileIndex;
  }

  private stepProfile(delta: number): void {
    const n = this.perProfile?.length ?? 0;
    if (n === 0) return;
    this.profileIndex = Math.min(n - 1, Math.max(0, this.profileIndex + delta));
    m.redraw();
  }

  // Navigates the un-merged flamegraph to the given profile (leaving merge mode
  // if needed); clicking a profile in the grid lands here.
  private jumpToProfile(
    attrs: AggregateProfilesMergePageAttrs,
    scope: string,
  ): void {
    const idField =
      attrs.columns.find((c) => c.kind === 'id')?.field ?? 'profile';
    const idx = this.workingRows(attrs).findIndex(
      (r) => String(r[idField]) === scope,
    );
    if (idx < 0) return;
    this.profileIndex = idx;
    if (attrs.state.merge) {
      attrs.onStateChange({...attrs.state, merge: false});
    }
    m.redraw();
  }

  // Left/right arrow keys step between profiles in the un-merged view, unless a
  // text field is focused (so typing in a filter isn't hijacked).
  private readonly onKeyDown = (e: KeyboardEvent): void => {
    if (this.merged || e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') return;
    const tag = (e.target as HTMLElement | null)?.tagName ?? '';
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
    if ((this.perProfile?.length ?? 0) <= 1) return;
    this.stepProfile(e.key === 'ArrowLeft' ? -1 : 1);
    e.preventDefault();
  };

  private renderCharts(
    attrs: AggregateProfilesMergePageAttrs,
    filters: ReadonlyArray<Filter>,
  ): m.Children {
    const byField = new Map(attrs.columns.map((c) => [c.field, c]));
    const shown = this.visibleColumns(attrs)
      .map((c) => byField.get(c.field))
      .filter((c): c is MergeColumn => c !== undefined && c.kind !== 'id');
    return m(
      '.pf-aggregate-merge__charts',
      shown.map((col) => this.renderChart(attrs, filters, col)),
    );
  }

  private renderChart(
    attrs: AggregateProfilesMergePageAttrs,
    filters: ReadonlyArray<Filter>,
    col: MergeColumn,
  ): m.Children {
    // Crossfilter: the distribution after every other column's filter, before
    // this column's own. Bins are raw totals (matching the range filters a
    // brush writes) over the profiles that pass the stack filters.
    const others = filters.filter((f) => f.field !== col.field);
    const base = applyFilters(this.participatingRows(attrs), others);
    let missing = 0;
    for (const r of base) {
      const v = r[col.field];
      if (v === null || v === undefined) missing++;
    }
    return m(
      '.pf-aggregate-merge__chart',
      m(
        '.pf-aggregate-merge__chart-title',
        col.title,
        // Profiles missing this sample-type go in the title, not a NULL bar.
        missing > 0 &&
          m('span.pf-aggregate-merge__chart-na', ` · ${missing} n/a`),
      ),
      this.renderHistogram(attrs, filters, col, base),
    );
  }

  private renderHistogram(
    attrs: AggregateProfilesMergePageAttrs,
    filters: ReadonlyArray<Filter>,
    col: MergeColumn,
    base: ReadonlyArray<Row>,
  ): m.Children {
    const values: number[] = [];
    for (const r of base) {
      const v = r[col.field];
      if (typeof v === 'number') values.push(v);
    }
    const data = computeHistogram(values, {bucketCount: 20});
    const range = rangeOf(filters, col.field);
    return m(HistogramSvg, {
      data,
      height: 96,
      formatXValue: (v: number) => displaySize(v, displayUnit(col.unit ?? '')),
      selection: range,
      onBrush: (rng: {start: number; end: number}) => {
        const start = Math.min(rng.start, rng.end);
        const end = Math.max(rng.start, rng.end);
        this.setFilters(
          attrs,
          replaceField(filters, col.field, [
            {field: col.field, op: '>=', value: start},
            {field: col.field, op: '<', value: end},
          ]),
        );
      },
    });
  }

  private renderGrid(
    attrs: AggregateProfilesMergePageAttrs,
    filters: ReadonlyArray<Filter>,
  ): m.Children {
    // In the un-merged view the profile shown below is highlighted in the grid,
    // and clicking any profile navigates the flamegraph to it.
    const current = attrs.state.merge
      ? undefined
      : this.perProfile?.[this.profileIndex]?.scope;
    const schema: ColumnSchema = {};
    for (const c of attrs.columns) {
      schema[c.field] = {
        title: c.title,
        columnType: c.kind === 'id' ? 'identifier' : 'quantitative',
        // Numeric cells are formatted exactly like the flamegraph formats the
        // same metric, so the two views always read consistently.
        cellRenderer:
          c.kind === 'numeric'
            ? (value: SqlValue) =>
                typeof value === 'number'
                  ? displaySize(value, displayUnit(c.unit ?? ''))
                  : ''
            : c.kind === 'id'
              ? (value: SqlValue) => {
                  const scope = String(value);
                  return m(
                    'span.pf-aggregate-merge__profile' +
                      (scope === current
                        ? '.pf-aggregate-merge__profile--current'
                        : ''),
                    {onclick: () => this.jumpToProfile(attrs, scope)},
                    scope,
                  );
                }
              : undefined,
      };
    }
    return m(DataGrid, {
      className: 'pf-aggregate-merge__grid',
      fillHeight: true,
      schema,
      data: this.gridData(attrs),
      columns: this.visibleColumns(attrs),
      onColumnsChanged: (columns: readonly Column[]) =>
        attrs.onStateChange({...attrs.state, columns: columns as Column[]}),
      filters,
      onFiltersChanged: (f: readonly Filter[]) =>
        attrs.onStateChange({...attrs.state, filters: f as Filter[]}),
    });
  }

  // The grid's data source. Under stack filters it holds only participating
  // profiles, with the filtered totals in the sample-type columns (display and
  // sorting follow the flamegraph) and the raw totals in shadow columns (the
  // grid's filters evaluate those, keeping brush ranges in raw units).
  private gridData(attrs: AggregateProfilesMergePageAttrs): MergeDataSource {
    const fv = this.filteredValues;
    const sig = `${this.filteredVer}`;
    if (this.gridSource === undefined || sig !== this.gridSourceSig) {
      this.gridSourceSig = sig;
      this.gridSource =
        fv === undefined
          ? new MergeDataSource(attrs.rows)
          : new MergeDataSource(...this.filteredRows(attrs, fv));
    }
    return this.gridSource;
  }

  // Whether a profile kept any samples under the flamegraph's stack filters.
  private participates(
    fv: ReadonlyMap<string, ReadonlyMap<string, number>>,
    scope: string,
  ): boolean {
    for (const byScope of fv.values()) {
      if ((byScope.get(scope) ?? 0) > 0) return true;
    }
    return false;
  }

  // The raw rows narrowed to participating profiles when a stack filter is
  // active (for the histograms, which bin raw values).
  private participatingRows(
    attrs: AggregateProfilesMergePageAttrs,
  ): ReadonlyArray<Row> {
    const fv = this.filteredValues;
    if (fv === undefined) return attrs.rows;
    const idField =
      attrs.columns.find((c) => c.kind === 'id')?.field ?? 'profile';
    return attrs.rows.filter((r) => this.participates(fv, String(r[idField])));
  }

  // Applies the filtered totals to the raw rows: drops non-participating
  // profiles, overrides each sample-type column and keeps the raw value in a
  // `<field>_raw` shadow. Returns the rows plus the display->shadow mapping.
  private filteredRows(
    attrs: AggregateProfilesMergePageAttrs,
    fv: ReadonlyMap<string, ReadonlyMap<string, number>>,
  ): [Row[], Array<{display: string; raw: string}>] {
    const idField =
      attrs.columns.find((c) => c.kind === 'id')?.field ?? 'profile';
    const sampleCols = attrs.columns.flatMap((c) =>
      c.sampleKey === undefined ? [] : [{field: c.field, key: c.sampleKey}],
    );
    const shadows = sampleCols.map((c) => ({
      display: c.field,
      raw: `${c.field}_raw`,
    }));
    const rows: Row[] = [];
    for (const r of attrs.rows) {
      const scope = String(r[idField]);
      if (!this.participates(fv, scope)) continue;
      const row: Row = {...r};
      for (const c of sampleCols) {
        row[`${c.field}_raw`] = r[c.field];
        row[c.field] =
          r[c.field] === null ? null : (fv.get(c.key)?.get(scope) ?? 0);
      }
      rows.push(row);
    }
    return [rows, shadows];
  }

  private renderFlamegraph(attrs: AggregateProfilesMergePageAttrs): m.Children {
    const rows = this.workingRows(attrs);
    const list = this.perProfile ?? [];
    const idx = this.clampIndex(list.length);
    const nav =
      !attrs.state.merge && list.length > 0
        ? m(
            '.pf-aggregate-merge__flame-nav',
            m(Button, {
              icon: 'chevron_left',
              compact: true,
              disabled: idx <= 0,
              title: 'Previous profile',
              onclick: () => this.stepProfile(-1),
            }),
            m('.pf-aggregate-merge__flame-pos', `${idx + 1} / ${list.length}`),
            m(Button, {
              icon: 'chevron_right',
              compact: true,
              disabled: idx >= list.length - 1,
              title: 'Next profile',
              onclick: () => this.stepProfile(1),
            }),
          )
        : null;
    const header = m(
      '.pf-aggregate-merge__flame-head',
      m(Switch, {
        label: 'Merge',
        checked: attrs.state.merge,
        onchange: () =>
          attrs.onStateChange({...attrs.state, merge: !attrs.state.merge}),
      }),
      m(
        '.pf-aggregate-merge__summary',
        `${attrs.state.merge ? 'Merging' : 'Showing'} ${rows.length} of ` +
          `${attrs.rows.length} profiles`,
      ),
      nav,
    );
    const body = attrs.state.merge
      ? this.renderMergedFlame(attrs)
      : this.renderPerProfileFlame(attrs, idx);
    return m(
      '.pf-aggregate-merge__flame',
      header,
      m('.pf-aggregate-merge__flame-body', body),
    );
  }

  private renderMergedFlame(
    attrs: AggregateProfilesMergePageAttrs,
  ): m.Children {
    if (this.flamegraphMetrics === undefined) {
      return m(EmptyState, {
        icon: 'filter_alt',
        title: 'No profiles match',
        detail: 'Adjust the filters to select profiles to merge.',
      });
    }
    return m(FlamegraphPanel, {
      trace: attrs.trace,
      metrics: this.flamegraphMetrics,
      state:
        this.flamegraphState ??
        Flamegraph.createDefaultState(this.flamegraphMetrics),
      onStateChange: (s) => {
        this.flamegraphState = s;
        attrs.onStateChange({...attrs.state, flamegraphState: s});
      },
    });
  }

  // One flamegraph at a time, for the profile at `idx` in the sorted/filtered
  // list; the header arrows (or left/right keys) move between them. Its state is
  // derived from the shared master, so filters and view carry across profiles.
  private renderPerProfileFlame(
    attrs: AggregateProfilesMergePageAttrs,
    idx: number,
  ): m.Children {
    const list = this.perProfile ?? [];
    if (list.length === 0) {
      return m(EmptyState, {
        icon: 'filter_alt',
        title: 'No profiles match',
        detail: 'Adjust the filters to select profiles.',
      });
    }
    this.syncPerProfileStates();
    const p = list[idx];
    const isUrl =
      p.scope.startsWith('http://') || p.scope.startsWith('https://');
    return m(
      '.pf-aggregate-merge__flame-single',
      m(
        '.pf-aggregate-merge__flame-cell-title',
        {title: p.scope},
        isUrl ? m(Anchor, {href: p.scope, target: '_blank'}, p.scope) : p.scope,
      ),
      m(
        '.pf-aggregate-merge__flame-cell-body',
        m(FlamegraphPanel, {
          trace: attrs.trace,
          metrics: p.metrics,
          state:
            this.profileStates.get(p.scope) ??
            Flamegraph.createDefaultState(p.metrics),
          // Propagate to the shared master so all profiles stay in sync.
          onStateChange: (s) => {
            this.flamegraphState = s;
            this.perProfileSig = undefined;
            attrs.onStateChange({...attrs.state, flamegraphState: s});
          },
        }),
      ),
    );
  }

  private setFilters(
    attrs: AggregateProfilesMergePageAttrs,
    filters: ReadonlyArray<Filter>,
  ): void {
    attrs.onStateChange({...attrs.state, filters: filters as Filter[]});
  }

  private filters(
    attrs: AggregateProfilesMergePageAttrs,
  ): ReadonlyArray<Filter> {
    return (attrs.state.filters ?? []) as Filter[];
  }

  private visibleColumns(attrs: AggregateProfilesMergePageAttrs): Column[] {
    const cols = attrs.state.columns as Column[] | undefined;
    if (cols && cols.length > 0) return cols;
    return attrs.columns.map((c) => ({id: c.field, field: c.field}));
  }

  // The filtered profiles that drive the flamegraph, in the grid's exact order:
  // the user's grid filters evaluate the raw totals, an active stack filter
  // drops non-participating profiles and swaps in the filtered totals (so a
  // sort on a sample-type column follows what the grid displays).
  private workingRows(
    attrs: AggregateProfilesMergePageAttrs,
  ): ReadonlyArray<Row> {
    const fv = this.filteredValues;
    let rows = attrs.rows;
    let filters = this.filters(attrs);
    if (fv !== undefined) {
      // Same remap the grid's data source does: the user's filters evaluate
      // the raw totals kept in the shadow columns.
      const [effRows, shadows] = this.filteredRows(attrs, fv);
      const rawField = new Map(shadows.map((s) => [s.display, s.raw]));
      rows = effRows;
      filters = filters.map((f) => {
        const raw = rawField.get(f.field);
        return raw === undefined ? f : {...f, field: raw};
      });
    }
    return sortRows(
      applyFilters(rows, filters),
      activeSort(this.visibleColumns(attrs)),
    );
  }

  // The flamegraph's active stack-filter chips (the ops that change the totals
  // mirrored into the grid), labelled exactly as the flamegraph labels them.
  // Removing one removes it from the flamegraph too.
  private stackTags(
    attrs: AggregateProfilesMergePageAttrs,
  ): Array<{label: string; onRemove: () => void}> | undefined {
    const st = this.flamegraphState;
    if (st === undefined) {
      return undefined;
    }
    const setState = (next: FlamegraphState) => {
      this.flamegraphState = next;
      this.perProfileSig = undefined;
      attrs.onStateChange({...attrs.state, flamegraphState: next});
    };
    const chips: Array<{label: string; onRemove: () => void}> = [];
    for (const f of st.filters) {
      if (f.kind !== 'SHOW_STACK' && f.kind !== 'HIDE_STACK') continue;
      chips.push({
        label: toTags({...st, filters: [f], view: {kind: 'TOP_DOWN'}})[0],
        onRemove: () =>
          setState({...st, filters: st.filters.filter((x) => x !== f)}),
      });
    }
    if (st.view.kind === 'PIVOT') {
      chips.push({
        label: toTags({...st, filters: []})[0],
        onRemove: () => setState({...st, view: {kind: 'TOP_DOWN'}}),
      });
    }
    return chips.length > 0 ? chips : undefined;
  }

  // The flamegraph's stack filters that change the total: SHOW_STACK / PIVOT
  // narrow it, HIDE_STACK removes stacks. (HIDE_FRAME / SHOW_FROM_FRAME only
  // restructure the tree, so they don't affect the per-profile totals.)
  private stackPatterns(): {show: string[]; hide: string[]} {
    const show: string[] = [];
    const hide: string[] = [];
    const st = this.flamegraphState;
    if (st !== undefined) {
      for (const f of st.filters) {
        if (f.kind === 'SHOW_STACK') show.push(f.filter);
        else if (f.kind === 'HIDE_STACK') hide.push(f.filter);
      }
      if (st.view.kind === 'PIVOT') show.push(st.view.pivot);
    }
    return {show, hide};
  }

  // Kicks off (once) the async recompute of per-profile filtered totals when
  // the stack filters change. A stack filter selects callstacks, so it applies
  // to every sample-type at once, whichever one the flamegraph is displaying.
  private updateFilteredTotals(attrs: AggregateProfilesMergePageAttrs): void {
    const {show, hide} = this.stackPatterns();
    const active = show.length > 0 || hide.length > 0;
    const sig = active ? JSON.stringify([show, hide]) : '';
    if (sig === this.filteredSig) return;
    this.filteredSig = sig;
    if (!active) {
      this.filteredValues = undefined;
      this.filteredVer++;
      return;
    }
    const aggIds = attrs.profiles.flatMap((p) =>
      Array.from(p.sampleTypes.values(), (st) => st.aggId),
    );
    const seq = ++this.filteredSeq;
    filteredTotals(attrs.trace.engine, aggIds, show, hide).then((map) => {
      if (seq !== this.filteredSeq) return; // superseded
      this.filteredValues = map;
      this.filteredVer++;
      m.redraw();
    });
  }

  private aggId(scope: string, sampleKey: string): number | undefined {
    return this.scopeToProfile?.get(scope)?.sampleTypes.get(sampleKey)?.aggId;
  }

  // Merge on: one merged flamegraph, a metric per sample-type in its dropdown.
  // Merge off: one flamegraph per filtered profile (see syncPerProfileStates).
  private rebuildFlamegraph(attrs: AggregateProfilesMergePageAttrs): void {
    const rows = this.workingRows(attrs);
    const idField =
      attrs.columns.find((c) => c.kind === 'id')?.field ?? 'profile';

    if (attrs.state.merge) {
      this.perProfile = undefined;
      const metrics: QueryFlamegraphMetric[] = [];
      for (const st of attrs.sampleTypes) {
        const ids: number[] = [];
        for (const r of rows) {
          const id = this.aggId(String(r[idField]), st.key);
          if (id !== undefined) ids.push(id);
        }
        // Name is the bare sample-type (no count) so the selection survives
        // filtering and can be mirrored when un-merging.
        if (ids.length > 0) {
          metrics.push(buildMergeMetric(st.key, st.unit, ids));
        }
      }
      if (metrics.length === 0) {
        this.flamegraphMetrics = undefined;
        this.flamegraphState = undefined;
        return;
      }
      this.flamegraphMetrics = metrics;
      const reconciled = Flamegraph.updateState(
        attrs.state.flamegraphState,
        metrics,
      );
      this.flamegraphState = reconciled;
      if (reconciled !== attrs.state.flamegraphState) {
        attrs.onStateChange({...attrs.state, flamegraphState: reconciled});
      }
      return;
    }

    this.flamegraphMetrics = undefined;
    const currentScope = this.perProfile?.[this.profileIndex]?.scope;
    const list: {scope: string; metrics: QueryFlamegraphMetric[]}[] = [];
    for (const r of rows) {
      const scope = String(r[idField]);
      const p = this.scopeToProfile?.get(scope);
      if (p === undefined) continue;
      const metrics: QueryFlamegraphMetric[] = [];
      for (const st of attrs.sampleTypes) {
        const met = p.sampleTypes.get(st.key);
        if (met) metrics.push(buildMergeMetric(st.key, st.unit, [met.aggId]));
      }
      if (metrics.length > 0) list.push({scope, metrics});
    }
    this.perProfile = list;
    this.perProfileSig = undefined;
    // Keep the viewed profile selected across list changes; if it no longer
    // participates, clampIndex falls back to a valid index.
    const keep = list.findIndex((p) => p.scope === currentScope);
    if (keep >= 0) this.profileIndex = keep;
  }

  // Derives each un-merged flamegraph's state from the shared master (same view
  // mode, frame filters, pivot and selected sample-type). Recomputed only when
  // the master changes, so the state objects stay identity-stable across renders
  // — otherwise every flamegraph re-queries each frame and never resolves.
  private syncPerProfileStates(): void {
    const master = this.flamegraphState;
    const sig =
      master === undefined
        ? ''
        : JSON.stringify([
            master.view,
            master.filters,
            master.selectedMetricId,
          ]);
    if (sig === this.perProfileSig) return;
    this.perProfileSig = sig;
    for (const p of this.perProfile ?? []) {
      if (master === undefined) {
        this.profileStates.set(
          p.scope,
          Flamegraph.createDefaultState(p.metrics),
        );
        continue;
      }
      const selectedMetricId = p.metrics.some(
        (mm) => mm.name === master.selectedMetricId,
      )
        ? master.selectedMetricId
        : p.metrics[0].name;
      this.profileStates.set(p.scope, {
        view: master.view,
        filters: master.filters,
        selectedMetricId,
        addedMetricIds: master.addedMetricIds,
        displayMode: master.displayMode,
      });
    }
  }
}

// In-memory grid source whose filters evaluate the shadow raw-value columns
// while sorting and display use the filtered values in the visible columns, so
// brush-range filters always mean raw units regardless of the stack filters.
class MergeDataSource extends InMemoryDataSource {
  private readonly shadows: ReadonlyMap<string, string>;

  constructor(
    rows: ReadonlyArray<Row>,
    shadows?: ReadonlyArray<{display: string; raw: string}>,
  ) {
    super(rows);
    this.shadows = new Map(shadows?.map((s) => [s.display, s.raw]));
  }

  private remap(model: DataSourceModel): DataSourceModel {
    if (this.shadows.size === 0 || model.filters === undefined) return model;
    return {
      ...model,
      filters: model.filters.map((f) => {
        const raw = this.shadows.get(f.field);
        return raw === undefined ? f : {...f, field: raw};
      }),
    };
  }

  useRows(model: DataSourceModel): DataSourceRows {
    return super.useRows(this.remap(model));
  }

  exportData(model: DataSourceModel): Promise<readonly Row[]> {
    return super.exportData(this.remap(model));
  }
}

// Applies the DataGrid's Filter[] to in-memory rows (for the working set and the
// crossfilter chart distributions).
function applyFilters(
  rows: ReadonlyArray<Row>,
  filters: ReadonlyArray<Filter>,
): ReadonlyArray<Row> {
  if (filters.length === 0) return rows;
  return rows.filter((row) =>
    filters.every((f) => matchFilter(row[f.field], f)),
  );
}

// The grid's active sort (single-column, keyed by field == column id), or
// undefined. Mirrors DataGrid, which sorts by the first column carrying a sort.
function activeSort(
  columns: ReadonlyArray<Column>,
): {field: string; asc: boolean} | undefined {
  const c = columns.find((x) => x.sort !== undefined);
  return c === undefined ? undefined : {field: c.field, asc: c.sort === 'ASC'};
}

// Sorts rows with the same null-aware, type-specific comparison as the
// DataGrid's in-memory data source, so the flamegraph order matches the grid.
function sortRows(
  rows: ReadonlyArray<Row>,
  sort: {field: string; asc: boolean} | undefined,
): ReadonlyArray<Row> {
  if (sort === undefined) return rows;
  const {field, asc} = sort;
  return [...rows].sort((ra, rb) => {
    const a = ra[field];
    const b = rb[field];
    if (a === null && b === null) return 0;
    if (a === null) return asc ? -1 : 1;
    if (b === null) return asc ? 1 : -1;
    if (typeof a === 'number' && typeof b === 'number') {
      return asc ? a - b : b - a;
    }
    if (typeof a === 'bigint' && typeof b === 'bigint') {
      return asc ? Number(a - b) : Number(b - a);
    }
    if (typeof a === 'string' && typeof b === 'string') {
      return asc ? a.localeCompare(b) : b.localeCompare(a);
    }
    const sa = String(a);
    const sb = String(b);
    return asc ? sa.localeCompare(sb) : sb.localeCompare(sa);
  });
}

function matchFilter(v: SqlValue, f: Filter): boolean {
  switch (f.op) {
    case '=':
      return v === f.value;
    case '!=':
      return v !== f.value;
    case '<':
      return typeof v === 'number' && v < Number(f.value);
    case '<=':
      return typeof v === 'number' && v <= Number(f.value);
    case '>':
      return typeof v === 'number' && v > Number(f.value);
    case '>=':
      return typeof v === 'number' && v >= Number(f.value);
    case 'in':
      return f.value.some((x) => x === v);
    case 'not in':
      return !f.value.some((x) => x === v);
    case 'is null':
      return v === null || v === undefined;
    case 'is not null':
      return v !== null && v !== undefined;
    case 'glob':
      return globMatch(String(v), String(f.value));
    case 'not glob':
      return !globMatch(String(v), String(f.value));
    default:
      return true;
  }
}

function globMatch(value: string, pattern: string): boolean {
  const re = new RegExp(
    '^' +
      pattern
        .replace(/[.+^${}()|[\]\\]/g, '\\$&')
        .replace(/\*/g, '.*')
        .replace(/\?/g, '.') +
      '$',
  );
  return re.test(value);
}

function replaceField(
  filters: ReadonlyArray<Filter>,
  field: string,
  additions: ReadonlyArray<Filter>,
): ReadonlyArray<Filter> {
  return [...filters.filter((f) => f.field !== field), ...additions];
}

function rangeOf(
  filters: ReadonlyArray<Filter>,
  field: string,
): {start: number; end: number} | undefined {
  let start: number | undefined;
  let end: number | undefined;
  for (const f of filters) {
    if (f.field !== field) continue;
    if (f.op === '>=') start = Number(f.value);
    else if (f.op === '<') end = Number(f.value);
  }
  return start !== undefined && end !== undefined ? {start, end} : undefined;
}

function sqlString(s: string): string {
  return `'${s.replace(/'/g, "''")}'`;
}

// Per-profile totals for every sample-type under the given stack filters. A
// stack filter keeps/removes whole callstacks containing a matching frame: a
// leaf sample survives iff its callsite descends (in the callsite forest) from
// a frame matching every SHOW pattern and from no HIDE pattern. Returns
// sample-type key -> scope -> total.
async function filteredTotals(
  engine: Engine,
  aggIds: ReadonlyArray<number>,
  show: ReadonlyArray<string>,
  hide: ReadonlyArray<string>,
): Promise<Map<string, Map<string, number>>> {
  const out = new Map<string, Map<string, number>>();
  if (aggIds.length === 0) return out;
  const ids = aggIds.join(',');
  const forest = `_pm_forest_${aggIds.length}_${show.length}_${hide.length}`;
  const surv = `_pm_surv_${aggIds.length}_${show.length}_${hide.length}`;
  // Leaf callsites whose callstack contains a frame matching `pattern`.
  const reach = (pattern: string) => `
    SELECT DISTINCT f.callsite_id
    FROM ${forest} f
    WHERE f.is_leaf AND f.id IN (
      SELECT node_id FROM graph_reachable_dfs!(
        (SELECT parent_id AS source_node_id, id AS dest_node_id
         FROM ${forest} WHERE parent_id IS NOT NULL),
        (SELECT id AS node_id FROM ${forest}
         WHERE IFNULL(name, '') REGEXP ${sqlString(userFilterToRegex(pattern))})))`;
  const showCte =
    show.length === 0
      ? `SELECT DISTINCT callsite_id FROM ${forest} WHERE is_leaf`
      : `SELECT callsite_id FROM (${show
          .map((p, i) => `SELECT callsite_id, ${i} AS p FROM (${reach(p)})`)
          .join(' UNION ALL ')})
         GROUP BY callsite_id HAVING count(DISTINCT p) = ${show.length}`;
  const hideExcept =
    hide.length === 0
      ? ''
      : `WHERE callsite_id NOT IN (${hide
          .map((p) => `SELECT callsite_id FROM (${reach(p)})`)
          .join(' UNION ')})`;
  try {
    await engine.query('include perfetto module callstacks.stack_profile');
    await engine.query('include perfetto module graphs.search');
    await engine.query(`
      CREATE PERFETTO TABLE ${forest} AS
      SELECT id, parent_id, callsite_id, name,
             is_leaf_function_in_callsite_frame AS is_leaf
      FROM _callstacks_for_stack_profile_samples!((
        SELECT callsite_id FROM __intrinsic_aggregate_sample
        WHERE aggregate_profile_id IN (${ids}) GROUP BY callsite_id))`);
    await engine.query(
      `CREATE PERFETTO TABLE ${surv} AS SELECT callsite_id FROM (${showCte}) ${hideExcept}`,
    );
    const res = await engine.query(`
      SELECT
        ap.scope AS scope,
        ap.sample_type_type || ' (' || ap.sample_type_unit || ')' AS key,
        sum(s.value) AS filtered
      FROM __intrinsic_aggregate_sample s
      JOIN __intrinsic_aggregate_profile ap ON ap.id = s.aggregate_profile_id
      WHERE s.aggregate_profile_id IN (${ids})
        AND s.callsite_id IN (SELECT callsite_id FROM ${surv})
      GROUP BY ap.scope, key`);
    for (
      const it = res.iter({scope: STR, key: STR, filtered: NUM_NULL});
      it.valid();
      it.next()
    ) {
      let byScope = out.get(it.key);
      if (byScope === undefined) {
        byScope = new Map();
        out.set(it.key, byScope);
      }
      byScope.set(it.scope, it.filtered ?? 0);
    }
  } finally {
    await engine.query(`DROP TABLE IF EXISTS ${surv}`);
    await engine.query(`DROP TABLE IF EXISTS ${forest}`);
  }
  return out;
}

// Flamegraph metric that merges the given profiles via `aggregate_profile_id IN
// (...)`. Same-named callsites combine in the layout, giving the true summed
// flamegraph.
function buildMergeMetric(
  name: string,
  unit: string,
  aggIds: ReadonlyArray<number>,
): QueryFlamegraphMetric {
  const ids = aggIds.join(',');
  return {
    name,
    unit: displayUnit(unit),
    provenance: 'DEFAULT',
    nameColumnLabel: 'Symbol',
    dependencySql: 'include perfetto module callstacks.stack_profile',
    statement: `
      WITH profile_samples AS MATERIALIZED (
        SELECT callsite_id, sum(sample.value) AS sample_value
        FROM __intrinsic_aggregate_sample sample
        WHERE sample.aggregate_profile_id IN (${ids})
        GROUP BY callsite_id
      )
      SELECT
        c.id,
        c.parent_id as parentId,
        c.name,
        c.mapping_name,
        c.source_file || ':' || c.line_number as source_location,
        cast_string!(c.inlined) AS inlined,
        CASE WHEN c.is_leaf_function_in_callsite_frame
          THEN coalesce(m.sample_value, 0)
          ELSE 0
        END AS value
      FROM _callstacks_for_stack_profile_samples!(profile_samples) AS c
      LEFT JOIN profile_samples AS m USING (callsite_id)
    `,
    unaggregatableProperties: [
      {name: 'mapping_name', displayName: 'Mapping'},
      {name: 'inlined', displayName: 'Inlined', isVisible: () => false},
    ],
    aggregatableProperties: [
      {
        name: 'source_location',
        displayName: 'Source Location',
        mergeAggregation: 'ONE_OR_SUMMARY',
      },
    ],
  };
}

// Maps a pprof sample-type unit onto the flamegraph's unit vocabulary
// (displaySize special-cases 'ns' and 'B'), so every view scales identically.
function displayUnit(unit: string): string {
  switch (unit.toLowerCase()) {
    case 'nanoseconds':
      return 'ns';
    case 'bytes':
      return 'B';
    default:
      return unit;
  }
}
