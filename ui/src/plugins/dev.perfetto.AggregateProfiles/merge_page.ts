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
import {Flamegraph, displaySize} from '../../widgets/flamegraph';
import type {FlamegraphState} from '../../widgets/flamegraph';
import {Anchor} from '../../widgets/anchor';
import {Switch} from '../../widgets/switch';
import {Button} from '../../widgets/button';
import {EmptyState} from '../../widgets/empty_state';
import {ResizeHandle} from '../../widgets/resize_handle';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import type {ColumnSchema} from '../../components/widgets/datagrid/datagrid_schema';
import type {Column, Filter} from '../../components/widgets/datagrid/model';
import type {Row, SqlValue} from '../../trace_processor/query_result';
import {Monitor} from '../../base/monitor';
import {aggregateProfileMetric, displayUnit} from './metrics';
import type {
  MergeColumn,
  MergeProfile,
  MergePageState,
  SampleType,
} from './types';

// Drag floor fallback until the grid header has been measured.
const MIN_GRID_HEIGHT = 64;

export interface AggregateProfilesMergePageAttrs {
  readonly trace: Trace;
  readonly profiles: ReadonlyArray<MergeProfile>;
  readonly sampleTypes: ReadonlyArray<SampleType>;
  readonly columns: ReadonlyArray<MergeColumn>;
  readonly rows: ReadonlyArray<Row>;
  readonly state: MergePageState;
  readonly onStateChange: (state: MergePageState) => void;
}

// A profile picker over a flamegraph: the grid's filters select the working
// set of profiles; merge on sums them into one flamegraph, merge off steps
// through them one at a time in the grid's order.
export class AggregateProfilesMergePage implements m.ClassComponent<AggregateProfilesMergePageAttrs> {
  private attrs?: AggregateProfilesMergePageAttrs;
  private source?: InMemoryDataSource;
  private profileByScope?: ReadonlyMap<string, MergeProfile>;

  // Rebuilt when the working set changes (see rebuild()). Metric arrays must
  // stay reference-stable across renders or the flamegraph refetches.
  private mergedMetrics?: QueryFlamegraphMetric[];
  private mergedCount = 0;
  private perProfile?: ReadonlyArray<{
    scope: string;
    metrics: QueryFlamegraphMetric[];
  }>;
  private profileIndex = 0;
  // One-entry cache keeping the shown profile's flamegraph state stable when
  // it has to diverge from the master (see profileState()).
  private shownState?: {
    master?: FlamegraphState;
    scope: string;
    state: FlamegraphState;
  };
  private readonly monitor = new Monitor([
    () => this.attrs?.state.filters,
    () => this.attrs?.state.merge,
    // Sort only affects the un-merged view (step order); ignore it merged.
    () => (this.attrs?.state.merge ? undefined : this.sortKey()),
  ]);

  private gridHeight = 300;
  private gridRowEl?: HTMLElement;

  oncreate(): void {
    window.addEventListener('keydown', this.onKeyDown);
  }

  onremove(): void {
    window.removeEventListener('keydown', this.onKeyDown);
  }

  view({attrs}: m.CVnode<AggregateProfilesMergePageAttrs>): m.Children {
    this.attrs = attrs;
    this.profileByScope ??= new Map(attrs.profiles.map((p) => [p.scope, p]));
    this.monitor.ifStateChanged(() => this.rebuild(attrs));

    return m(
      '.pf-aggregate-merge',
      m(
        '.pf-aggregate-merge__row',
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
        '.pf-aggregate-merge__row.pf-aggregate-merge__row--grow',
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

  private renderGrid(attrs: AggregateProfilesMergePageAttrs): m.Children {
    const current = attrs.state.merge
      ? undefined
      : this.perProfile?.[this.profileIndex]?.scope;
    const schema: ColumnSchema = {};
    for (const c of attrs.columns) {
      schema[c.field] = {
        title: c.title,
        columnType:
          c.kind === 'id'
            ? 'identifier'
            : c.kind === 'numeric'
              ? 'quantitative'
              : 'text',
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
      data: attrs.rows as Row[],
      columns: this.columns(attrs),
      onColumnsChanged: (columns: readonly Column[]) =>
        attrs.onStateChange({...attrs.state, columns: columns as Column[]}),
      filters: this.filters(attrs),
      onFiltersChanged: (f: readonly Filter[]) =>
        attrs.onStateChange({...attrs.state, filters: f as Filter[]}),
    });
  }

  private renderFlamegraph(attrs: AggregateProfilesMergePageAttrs): m.Children {
    const count = attrs.state.merge
      ? this.mergedCount
      : (this.perProfile?.length ?? 0);
    const idx = this.clampIndex(this.perProfile?.length ?? 0);
    const nav =
      !attrs.state.merge && count > 0
        ? m(
            '.pf-aggregate-merge__flame-nav',
            m(Button, {
              icon: 'chevron_left',
              compact: true,
              disabled: idx <= 0,
              title: 'Previous profile',
              onclick: () => this.stepProfile(-1),
            }),
            m('.pf-aggregate-merge__flame-pos', `${idx + 1} / ${count}`),
            m(Button, {
              icon: 'chevron_right',
              compact: true,
              disabled: idx >= count - 1,
              title: 'Next profile',
              onclick: () => this.stepProfile(1),
            }),
          )
        : null;
    return m(
      '.pf-aggregate-merge__flame',
      m(
        '.pf-aggregate-merge__flame-head',
        m(Switch, {
          label: 'Merge',
          checked: attrs.state.merge,
          onchange: () =>
            attrs.onStateChange({...attrs.state, merge: !attrs.state.merge}),
        }),
        m(
          '.pf-aggregate-merge__summary',
          `${attrs.state.merge ? 'Merging' : 'Showing'} ${count} of ` +
            `${attrs.rows.length} profiles`,
        ),
        nav,
      ),
      m(
        '.pf-aggregate-merge__flame-body',
        attrs.state.merge
          ? this.renderMergedFlame(attrs)
          : this.renderProfileFlame(attrs, idx),
      ),
    );
  }

  private renderMergedFlame(
    attrs: AggregateProfilesMergePageAttrs,
  ): m.Children {
    const metrics = this.mergedMetrics;
    if (metrics === undefined) {
      return this.renderEmpty();
    }
    return m(FlamegraphPanel, {
      trace: attrs.trace,
      metrics,
      state: Flamegraph.updateState(attrs.state.flamegraphState, metrics),
      onStateChange: (s) =>
        attrs.onStateChange({...attrs.state, flamegraphState: s}),
    });
  }

  private renderProfileFlame(
    attrs: AggregateProfilesMergePageAttrs,
    idx: number,
  ): m.Children {
    const p = this.perProfile?.[idx];
    if (p === undefined) {
      return this.renderEmpty();
    }
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
          state: this.profileState(attrs, p),
          onStateChange: (s) =>
            attrs.onStateChange({...attrs.state, flamegraphState: s}),
        }),
      ),
    );
  }

  private renderEmpty(): m.Children {
    return m(EmptyState, {
      icon: 'filter_alt',
      title: 'No profiles match',
      detail: 'Adjust the filters to select profiles.',
    });
  }

  // The shown profile's flamegraph state: the shared master state, with the
  // selected metric swapped out if this profile lacks it. Cached so the state
  // object is reference-stable across renders.
  private profileState(
    attrs: AggregateProfilesMergePageAttrs,
    p: {scope: string; metrics: QueryFlamegraphMetric[]},
  ): FlamegraphState {
    const master = attrs.state.flamegraphState;
    const cached = this.shownState;
    if (
      cached !== undefined &&
      cached.master === master &&
      cached.scope === p.scope
    ) {
      return cached.state;
    }
    const state = Flamegraph.updateState(master, p.metrics);
    this.shownState = {master, scope: p.scope, state};
    return state;
  }

  private filters(attrs: AggregateProfilesMergePageAttrs): readonly Filter[] {
    return (attrs.state.filters ?? []) as Filter[];
  }

  private columns(attrs: AggregateProfilesMergePageAttrs): Column[] {
    const cols = attrs.state.columns as Column[] | undefined;
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

  // The filtered profiles driving the flamegraph, in the grid's sort order.
  // Evaluated by the same DataGrid machinery that renders the grid.
  private workingRows(
    attrs: AggregateProfilesMergePageAttrs,
  ): ReadonlyArray<Row> {
    this.source ??= new InMemoryDataSource(attrs.rows);
    const sorted = this.columns(attrs).find((c) => c.sort !== undefined);
    const result = this.source.useRows({
      mode: 'flat',
      columns: attrs.columns.map((c) => ({field: c.field, alias: c.field})),
      filters: this.filters(attrs),
      sort: sorted?.sort
        ? {alias: sorted.field, direction: sorted.sort}
        : undefined,
    });
    return result.rows ?? [];
  }

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

  // Clicking a profile in the grid shows its flamegraph, leaving merge mode
  // if needed.
  private jumpToProfile(
    attrs: AggregateProfilesMergePageAttrs,
    scope: string,
  ): void {
    if (attrs.state.merge) {
      attrs.onStateChange({...attrs.state, merge: false});
      this.rebuild({...attrs, state: {...attrs.state, merge: false}});
    }
    const idx = this.perProfile?.findIndex((p) => p.scope === scope) ?? -1;
    if (idx >= 0) {
      this.profileIndex = idx;
    }
    m.redraw();
  }

  // Left/right arrows step between profiles in the un-merged view, unless a
  // text field is focused.
  private readonly onKeyDown = (e: KeyboardEvent): void => {
    if (this.attrs?.state.merge !== false) return;
    if (e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') return;
    const tag = (e.target as HTMLElement | null)?.tagName ?? '';
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
    if ((this.perProfile?.length ?? 0) <= 1) return;
    this.stepProfile(e.key === 'ArrowLeft' ? -1 : 1);
    e.preventDefault();
  };

  // Merge on: one flamegraph, a metric per sample-type, each summing the
  // working set's aggregate profiles. Merge off: one metric set per profile.
  private rebuild(attrs: AggregateProfilesMergePageAttrs): void {
    const rows = this.workingRows(attrs);
    const idField = attrs.columns.find((c) => c.kind === 'id')?.field ?? 'c0';
    const profileOf = (row: Row) =>
      this.profileByScope?.get(String(row[idField]));

    if (attrs.state.merge) {
      this.perProfile = undefined;
      const metrics: QueryFlamegraphMetric[] = [];
      for (const st of attrs.sampleTypes) {
        const ids = rows
          .map((r) => profileOf(r)?.sampleTypes.get(st.key)?.aggId)
          .filter((id) => id !== undefined);
        if (ids.length > 0) {
          metrics.push(aggregateProfileMetric(st.key, st.unit, ids));
        }
      }
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
    const shownScope = this.perProfile?.[this.profileIndex]?.scope;
    this.perProfile = rows.flatMap((row) => {
      const p = profileOf(row);
      if (p === undefined) return [];
      const metrics = attrs.sampleTypes.flatMap((st) => {
        const met = p.sampleTypes.get(st.key);
        return met
          ? [aggregateProfileMetric(st.key, st.unit, [met.aggId])]
          : [];
      });
      return metrics.length > 0 ? [{scope: p.scope, metrics}] : [];
    });
    // Keep the shown profile selected across working-set changes.
    const keep = this.perProfile.findIndex((p) => p.scope === shownScope);
    if (keep >= 0) {
      this.profileIndex = keep;
    }
  }
}
