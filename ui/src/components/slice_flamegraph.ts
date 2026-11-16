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

// Reusable slice flamegraph: aggregates a set of slices by their nested slice
// tree (parent -> child slice names), weighted by self duration. Unlike the
// Track Event callstack flamegraph it works for any slices (ftrace/atrace as
// well as SDK track events), since it uses the slice hierarchy rather than
// captured code call stacks.
//
// The caller supplies `nodesSql`, a query yielding the node set of the tree as
// (id, dur, name, parent_id) rows: the roots are the slices whose parent_id is
// not itself in the set. The component materializes that set, indexes it and
// runs viz.slices' `_viz_slice_ancestor_agg` over it. The area-selection
// "Slice Flamegraph" tab is the same aggregation over a time window; this lets
// other surfaces (e.g. the slice value-distribution panel) reuse it over an
// arbitrary set of slices.

import m from 'mithril';
import {AsyncDisposableStack} from '../base/disposable_stack';
import {AsyncMemo, AtomicTaskQueue} from '../base/async_memo';
import type {Trace} from '../public/trace';
import {
  createPerfettoIndex,
  createPerfettoTable,
} from '../trace_processor/sql_utils';
import {Spinner} from '../widgets/spinner';
import {
  type TreeExplorerOptionalAction,
  updateTreeExplorerState,
  type TreeExplorerState,
} from '../widgets/tree_explorer';
import {TreeExplorerPanel} from './tree_explorer_panel';
import {
  metricsFromTableOrSubquery,
  TreeExplorerFetcher,
} from './tree_explorer_fetcher';

// Builds the flamegraph metrics (self duration + slice count) from a
// materialized node-set table, using viz.slices' ancestor aggregation.
// `optionalActions`, if given, are the drill-down actions offered on each
// flamegraph node (e.g. open the matching slices in a new tab).
export function buildSliceFlamegraphMetrics(
  nodesTable: string,
  optionalActions?: ReadonlyArray<TreeExplorerOptionalAction>,
) {
  return metricsFromTableOrSubquery({
    optionalActions,
    tableOrSubquery: `(
      SELECT *
      FROM _viz_slice_ancestor_agg!(
        (
          SELECT s.id, s.dur
          FROM ${nodesTable} s
          LEFT JOIN ${nodesTable} t ON t.parent_id = s.id
          WHERE t.id IS NULL
        ),
        ${nodesTable}
      )
    )`,
    tableMetrics: [
      {
        name: 'Duration',
        unit: 'ns',
        columnName: 'self_dur',
      },
      {
        name: 'Samples',
        unit: '',
        columnName: 'self_count',
      },
    ],
    dependencySql: 'include perfetto module viz.slices;',
    aggregatableProperties: [
      {
        name: 'simple_count',
        displayName: 'Slice Count',
        mergeAggregation: 'SUM',
        isVisible: (_) => true,
      },
    ],
    nameColumnLabel: 'Slice Name',
  });
}

interface SliceFlamegraphResources extends AsyncDisposable {
  readonly fetcher: TreeExplorerFetcher;
}

async function buildSliceFlamegraphResources(opts: {
  readonly trace: Trace;
  readonly nodesSql: string;
  readonly queue: AtomicTaskQueue;
  readonly dependencySql?: string;
  readonly optionalActions?: ReadonlyArray<TreeExplorerOptionalAction>;
}): Promise<SliceFlamegraphResources> {
  const {trace, nodesSql, queue, dependencySql, optionalActions} = opts;
  // Whatever `nodesSql` needs (e.g. a stdlib module it calls a macro from).
  // Includes are idempotent and persist on the engine, so drill-downs spawned
  // from the flamegraph can reuse the same node-set query later on.
  if (dependencySql !== undefined) {
    await trace.engine.query(dependencySql);
  }
  await using disposables = new AsyncDisposableStack();
  const nodes = disposables.use(
    await createPerfettoTable({engine: trace.engine, as: nodesSql}),
  );
  // The index speeds up the leaf lookup and the aggregation's parent walk. It
  // is automatically dropped when the table it is built on is dropped.
  await createPerfettoIndex({
    engine: trace.engine,
    on: `${nodes.name}(parent_id)`,
  });

  const metrics = buildSliceFlamegraphMetrics(nodes.name, optionalActions);
  // The fetcher is created next to the metrics it serves and moved into the
  // returned object, so its virtual tables (which read from the node table)
  // never outlive that table.
  const fetcher = new TreeExplorerFetcher(trace, metrics, queue);
  const owned = disposables.move();
  return {
    fetcher,
    [Symbol.asyncDispose]: async () => {
      // Dispose the fetcher first: its virtual tables are built from the table
      // `owned` drops. Its disposal is queued, so it lands after any in-flight
      // query against those tables.
      fetcher[Symbol.dispose]();
      await owned[Symbol.asyncDispose]();
    },
  };
}

export interface SliceFlamegraphAttrs {
  readonly trace: Trace;

  // Query yielding the node set as (id, dur, name, parent_id) rows. The roots
  // of the flamegraph are the nodes whose parent_id is not present in the set.
  // Changing it (e.g. because the caller's filter changed) transparently
  // rebuilds the flamegraph.
  readonly nodesSql: string;

  // Run once before `nodesSql`, for whatever it depends on (typically
  // `include perfetto module ...` statements).
  readonly dependencySql?: string;

  // Drill-down actions offered on each flamegraph node. They are derived from
  // (and change together with) `nodesSql`, so keying the rebuild on `nodesSql`
  // alone keeps them in sync.
  readonly optionalActions?: ReadonlyArray<TreeExplorerOptionalAction>;
}

export class SliceFlamegraph implements m.ClassComponent<SliceFlamegraphAttrs> {
  private readonly queue = new AtomicTaskQueue();
  private readonly resourcesSlot = new AsyncMemo<SliceFlamegraphResources>(
    this.queue,
  );
  private state?: TreeExplorerState;

  view({attrs}: m.CVnode<SliceFlamegraphAttrs>): m.Children {
    const {isPending, data} = this.resourcesSlot.use({
      key: {nodesSql: attrs.nodesSql},
      compute: () =>
        buildSliceFlamegraphResources({
          trace: attrs.trace,
          nodesSql: attrs.nodesSql,
          queue: this.queue,
          dependencySql: attrs.dependencySql,
          optionalActions: attrs.optionalActions,
        }),
    });
    if (data === undefined) {
      return isPending ? m(Spinner, {easing: true}) : undefined;
    }
    this.state = updateTreeExplorerState(this.state, data.fetcher.metrics);
    return m(TreeExplorerPanel, {
      fetcher: data.fetcher,
      state: this.state,
      onStateChange: (state) => {
        this.state = state;
      },
    });
  }

  onremove(): void {
    this.resourcesSlot.dispose();
  }
}
