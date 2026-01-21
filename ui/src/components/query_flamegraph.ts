// Copyright (C) 2024 The Android Open Source Project
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
import {AsyncLimiter} from '../base/async_limiter';
import {AsyncDisposableStack} from '../base/disposable_stack';
import {assertExists} from '../base/logging';
import {uuidv4Sql} from '../base/uuid';
import {Engine} from '../trace_processor/engine';
import {createPerfettoTable} from '../trace_processor/sql_utils';
import {
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
  UNKNOWN,
} from '../trace_processor/query_result';
import {
  Flamegraph,
  FlamegraphPropertyDefinition,
  FlamegraphQueryData,
  FlamegraphState,
  FlamegraphOptionalAction,
  FlamegraphOptionalMarker,
} from '../widgets/flamegraph';
import {Trace} from '../public/trace';
import {SharedAsyncDisposable} from '../base/shared_disposable';
import {Monitor} from '../base/monitor';

export interface QueryFlamegraphColumn {
  // The name of the column in SQL.
  readonly name: string;

  // The human readable name describing the contents of the column.
  readonly displayName: string;

  // Function that determines whether the property should be displayed for a
  // given node.
  readonly isVisible?: (value: string) => boolean;
}

export interface AggQueryFlamegraphColumn extends QueryFlamegraphColumn {
  // The aggregation to be run when nodes are merged together in the flamegraph.
  readonly mergeAggregation: 'ONE_OR_SUMMARY' | 'SUM' | 'CONCAT_WITH_COMMA';
}

export interface QueryFlamegraphMetric {
  // The human readable name of the metric: will be shown to the user to change
  // between metrics.
  readonly name: string;

  // The human readable SI-style unit of `selfValue`. Values will be shown to
  // the user suffixed with this.
  readonly unit: string;

  // Label for the name column in copy stack table and tooltip.
  // Examples: "Symbol", "Slice", "Class". Defaults to "Name".
  readonly nameColumnLabel?: string;

  // SQL statement which need to be run in preparation for being able to execute
  // `statement`.
  readonly dependencySql?: string;

  // A single SQL statement which returns the columns `id`, `parentId`, `name`
  // `selfValue`, all columns specified by `unaggregatableProperties` and
  // `aggregatableProperties`.
  readonly statement: string;

  // Additional contextual columns containing data which should not be merged
  // between sibling nodes, even if they have the same name.
  //
  // Examples include the mapping that a name comes from, the heap graph root
  // type etc.
  //
  // Note: the name is always unaggregatable and should not be specified here.
  readonly unaggregatableProperties?: ReadonlyArray<QueryFlamegraphColumn>;

  // Additional contextual columns containing data which will be displayed to
  // the user if there is no merging. If there is merging, currently the value
  // will not be shown.
  //
  // Examples include the source file and line number.
  readonly aggregatableProperties?: ReadonlyArray<AggQueryFlamegraphColumn>;

  // Optional actions to be taken on the flamegraph nodes. Accessible from the
  // flamegraph tooltip.
  //
  // Examples include showing a table of objects from a class reference
  // hierarchy.
  readonly optionalNodeActions?: ReadonlyArray<FlamegraphOptionalAction>;

  // Optional actions to be taken on the flamegraph root. Accessible from the
  // flamegraph tooltip.
  //
  // Examples include showing a table of objects from a class reference
  // hierarchy.
  readonly optionalRootActions?: ReadonlyArray<FlamegraphOptionalAction>;

  // Optional marker to be displayed on flamegraph nodes. Marker appears as
  // a visual indicator (small dot) on the left side of nodes and is shown
  // in the tooltip.
  //
  // Examples include marking inlined functions, optimized code, etc.
  readonly optionalMarker?: FlamegraphOptionalMarker;
}

export interface MetricsFromTableOrSubqueryOptions {
  readonly tableOrSubquery: string;
  readonly tableMetrics: ReadonlyArray<{
    name: string;
    unit: string;
    columnName: string;
  }>;
  readonly dependencySql?: string;
  readonly unaggregatableProperties?: ReadonlyArray<QueryFlamegraphColumn>;
  readonly aggregatableProperties?: ReadonlyArray<AggQueryFlamegraphColumn>;
  readonly optionalActions?: ReadonlyArray<FlamegraphOptionalAction>;
  readonly nameColumnLabel?: string;
}

// Given a table and columns on those table (corresponding to metrics),
// returns an array of `QueryFlamegraphMetric` structs which can be passed
// in QueryFlamegraph's attrs.
//
// `tableOrSubquery` should have the columns `id`, `parentId`, `name` and all
// columns specified by `tableMetrics[].name`, `unaggregatableProperties` and
// `aggregatableProperties`.
export function metricsFromTableOrSubquery(
  opts: MetricsFromTableOrSubqueryOptions,
): QueryFlamegraphMetric[] {
  const metrics = [];
  for (const {name, unit, columnName} of opts.tableMetrics) {
    metrics.push({
      name,
      unit,
      nameColumnLabel: opts.nameColumnLabel,
      dependencySql: opts.dependencySql,
      statement: `
        select *, ${columnName} as value
        from ${opts.tableOrSubquery}
      `,
      unaggregatableProperties: opts.unaggregatableProperties,
      aggregatableProperties: opts.aggregatableProperties,
      optionalActions: opts.optionalActions,
    });
  }
  return metrics;
}

interface QueryFlamegraphAttrs {
  // The metrics to display in the flamegraph. If undefined, the flamegraph will
  // show a loading state.
  readonly metrics?: ReadonlyArray<QueryFlamegraphMetric>;

  // The current state of the flamegraph (filters, view, selected metric, etc).
  readonly state?: FlamegraphState;

  // Callback invoked when the flamegraph state changes (e.g., user changes
  // filters, selects a different metric, etc).
  readonly onStateChange: (state: FlamegraphState) => void;
}

export interface QueryFlamegraphWithMetrics {
  flamegraph: QueryFlamegraph;
  metrics: ReadonlyArray<QueryFlamegraphMetric>;
}

// A Perfetto UI component which wraps the `Flamegraph` widget and fetches the
// data for the widget by querying an `Engine`.
export class QueryFlamegraph implements AsyncDisposable {
  private data?: FlamegraphQueryData;
  private readonly queryLimiter = new AsyncLimiter();
  private readonly dependencies: ReadonlyArray<
    SharedAsyncDisposable<AsyncDisposable>
  >;
  private lastAttrs?: QueryFlamegraphAttrs;
  private monitor = new Monitor([
    () => this.lastAttrs?.metrics,
    () => this.lastAttrs?.state,
  ]);

  constructor(
    private readonly trace: Trace,
    dependencies: ReadonlyArray<AsyncDisposable> = [],
  ) {
    this.dependencies = dependencies.map((d) => SharedAsyncDisposable.wrap(d));
  }

  async [Symbol.asyncDispose](): Promise<void> {
    for (const dependency of this.dependencies ?? []) {
      await dependency[Symbol.asyncDispose]?.();
    }
  }

  render(attrs: QueryFlamegraphAttrs) {
    const {metrics, state, onStateChange} = attrs;
    this.lastAttrs = attrs;
    if (this.monitor.ifStateChanged()) {
      this.data = undefined;
      if (metrics && state) {
        this.fetchData(metrics, state);
      }
    }
    return m(Flamegraph, {
      metrics: metrics ?? [],
      data: this.data,
      state: state ?? {
        view: {kind: 'TOP_DOWN'},
        selectedMetricName: '',
        filters: [],
      },
      onStateChange,
    });
  }

  fetchData(
    metrics: ReadonlyArray<QueryFlamegraphMetric>,
    state: FlamegraphState,
  ) {
    const metric = assertExists(
      metrics.find((x) => state.selectedMetricName === x.name),
    );
    const engine = this.trace.engine;
    this.queryLimiter.schedule(async () => {
      this.data = undefined;
      // Clone all the dependencies to make sure the the are not dropped while
      // this function is running, adding them to the trash to make sure they
      // are disposed after this function returns, but note this won't
      // actually drop the tables unless this class instances have also been
      // disposed due to the SharedAsyncDisposable logic.
      await using trash = new AsyncDisposableStack();
      for (const dependency of this.dependencies ?? []) {
        // If the dependency is disposed, it means that we have already ended
        // up cleaning up the object so none of this matters. Just return.
        if (dependency.isDisposed) {
          return;
        }
        trash.use(dependency.clone());
      }
      this.data = await computeFlamegraphTree(engine, metric, state);
    });
  }
}

async function computeFlamegraphTree(
  engine: Engine,
  {
    dependencySql,
    statement,
    unaggregatableProperties,
    aggregatableProperties,
    optionalNodeActions,
    optionalRootActions,
    optionalMarker,
  }: QueryFlamegraphMetric,
  {filters, view}: FlamegraphState,
): Promise<FlamegraphQueryData> {
  // TODO(flamegraph2): Add filtering support using tree_delete_node!
  void filters;

  const agg = aggregatableProperties ?? [];
  const aggCols = agg.map((x) => x.name);
  const unagg = unaggregatableProperties ?? [];
  const unaggCols = unagg.map((x) => x.name);

  const nodeActions = optionalNodeActions ?? [];
  const rootActions = optionalRootActions ?? [];

  // Build the list of all user columns for tree operations
  const allUserCols = ['name', 'value', ...unaggCols, ...aggCols];

  // Build aggregation expressions for tree_merge_siblings
  // name is the key, value is always summed, other columns need their aggregation type
  const aggExprs = ['tree_agg!(value, SUM)'];
  for (const a of agg) {
    switch (a.mergeAggregation) {
      case 'SUM':
        aggExprs.push(`tree_agg!(${a.name}, SUM)`);
        break;
      case 'ONE_OR_SUMMARY':
      case 'CONCAT_WITH_COMMA':
        // For these, we use ANY since tree algebra doesn't have these modes
        aggExprs.push(`tree_agg!(${a.name}, ANY)`);
        break;
    }
  }
  // Add ANY aggregation for unaggregatable columns to pass them through
  for (const u of unaggCols) {
    aggExprs.push(`tree_agg!(${u}, ANY)`);
  }

  if (dependencySql !== undefined) {
    await engine.query(dependencySql);
  }
  await engine.query(`include perfetto module viz.flamegraph2;`);
  await engine.query(`include perfetto module std.trees.from_table;`);
  await engine.query(`include perfetto module std.trees.merge;`);
  await engine.query(`include perfetto module std.trees.invert;`);
  await engine.query(`include perfetto module std.trees.propagate;`);
  await engine.query(`include perfetto module std.trees.to_table;`);

  const uuid = uuidv4Sql();
  await using disposable = new AsyncDisposableStack();

  // Create source table from metric statement
  disposable.use(
    await createPerfettoTable({
      engine,
      name: `_flamegraph_source_${uuid}`,
      as: `
        SELECT
          id,
          parentId AS parent_id,
          name,
          value
          ${unaggCols.length === 0 ? '' : ', ' + unaggCols.join(', ')}
          ${aggCols.length === 0 ? '' : ', ' + aggCols.join(', ')}
        FROM (${statement})
      `,
    }),
  );

  // Build the tree operations based on view type
  // For top-down: merge siblings, then propagate cumulative values
  // For bottom-up: invert tree, then merge, then propagate
  const isBottomUp = view.kind === 'BOTTOM_UP';

  // Build merge key - includes name and all unaggregatable columns
  const mergeKeyColumns =
    unaggCols.length === 0 ? 'name' : ['name', ...unaggCols].join(', ');
  const mergeKeyExpr =
    unaggCols.length === 0
      ? 'tree_key!(name)'
      : `tree_keys!((${mergeKeyColumns}))`;

  const userColumnsExpr = `(${allUserCols.join(', ')})`;

  // Build the tree expression based on view type
  let treeExpr: string;
  if (isBottomUp) {
    // Bottom-up: invert first, then merge
    treeExpr = `
      tree_propagate_up!(
        tree_invert!(
          tree_from_table!(
            _flamegraph_source_${uuid},
            id,
            parent_id,
            ${userColumnsExpr}
          ),
          ${mergeKeyExpr},
          tree_order!(value),
          ${aggExprs.join(', ')}
        ),
        tree_propagate_spec!(cumulative_value, value, SUM)
      )
    `;
  } else {
    // Top-down: merge siblings, then propagate
    treeExpr = `
      tree_propagate_up!(
        tree_merge_siblings!(
          tree_from_table!(
            _flamegraph_source_${uuid},
            id,
            parent_id,
            ${userColumnsExpr}
          ),
          tree_merge_mode!(GLOBAL),
          ${mergeKeyExpr},
          tree_order!(value),
          ${aggExprs.join(', ')}
        ),
        tree_propagate_spec!(cumulative_value, value, SUM)
      )
    `;
  }

  // Create the merged tree table
  const outputCols = [...allUserCols, 'cumulative_value'];
  disposable.use(
    await createPerfettoTable({
      engine,
      name: `_flamegraph_merged_${uuid}`,
      as: `
        SELECT
          __node_id AS id,
          __parent_id AS parent_id,
          __depth AS depth,
          ${outputCols.join(', ')}
        FROM tree_to_table!(
          ${treeExpr},
          (${outputCols.join(', ')})
        )
        WHERE cumulative_value > 0
      `,
    }),
  );

  // Compute local layout (xStart, xEnd relative to parent)
  disposable.use(
    await createPerfettoTable({
      engine,
      name: `_flamegraph_layout_${uuid}`,
      as: `
        SELECT
          id,
          COALESCE(
            SUM(cumulative_value) OVER (
              PARTITION BY parent_id
              ORDER BY cumulative_value DESC
              ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
            ),
            0
          ) AS xStart,
          SUM(cumulative_value) OVER (
            PARTITION BY parent_id
            ORDER BY cumulative_value DESC
            ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
          ) AS xEnd
        FROM _flamegraph_merged_${uuid}
      `,
    }),
  );

  // Compute global layout by accumulating parent offsets
  // This uses a recursive CTE to propagate xStart offsets down the tree
  const res = await engine.query(`
    WITH RECURSIVE
    global_layout AS (
      -- Base case: root nodes (parent_id IS NULL)
      SELECT
        m.id,
        m.parent_id,
        IIF(m.depth < 0, m.depth, m.depth + 1) AS depth,
        m.name,
        ${unaggCols.length > 0 ? unaggCols.map((c) => `m.${c}`).join(', ') + ',' : ''}
        ${aggCols.length > 0 ? aggCols.map((c) => `m.${c}`).join(', ') + ',' : ''}
        m.value AS selfValue,
        m.cumulative_value AS cumulativeValue,
        NULL AS parentCumulativeValue,
        l.xStart,
        l.xEnd
      FROM _flamegraph_merged_${uuid} m
      JOIN _flamegraph_layout_${uuid} l ON m.id = l.id
      WHERE m.parent_id IS NULL

      UNION ALL

      -- Recursive case: children
      SELECT
        m.id,
        m.parent_id,
        IIF(m.depth < 0, m.depth, m.depth + 1) AS depth,
        m.name,
        ${unaggCols.length > 0 ? unaggCols.map((c) => `m.${c}`).join(', ') + ',' : ''}
        ${aggCols.length > 0 ? aggCols.map((c) => `m.${c}`).join(', ') + ',' : ''}
        m.value AS selfValue,
        m.cumulative_value AS cumulativeValue,
        g.cumulativeValue AS parentCumulativeValue,
        g.xStart + l.xStart AS xStart,
        g.xStart + l.xEnd AS xEnd
      FROM _flamegraph_merged_${uuid} m
      JOIN _flamegraph_layout_${uuid} l ON m.id = l.id
      JOIN global_layout g ON m.parent_id = g.id
    )
    SELECT
      id,
      IFNULL(parent_id, -1) AS parentId,
      depth,
      IIF(name = '', 'unknown', name) AS name,
      ${unaggCols.length > 0 ? unaggCols.join(', ') + ',' : ''}
      ${aggCols.length > 0 ? aggCols.join(', ') + ',' : ''}
      selfValue,
      cumulativeValue,
      parentCumulativeValue,
      xStart,
      xEnd
    FROM global_layout
    ORDER BY ABS(depth), xStart
  `);

  const it = res.iter({
    id: NUM,
    parentId: NUM,
    depth: NUM,
    name: STR,
    selfValue: NUM,
    cumulativeValue: NUM,
    parentCumulativeValue: NUM_NULL,
    xStart: NUM,
    xEnd: NUM,
    ...Object.fromEntries(unaggCols.map((m) => [m, STR_NULL])),
    ...Object.fromEntries(aggCols.map((m) => [m, UNKNOWN])),
  });
  let postiveRootsValue = 0;
  let negativeRootsValue = 0;
  let minDepth = 0;
  let maxDepth = 0;
  const nodes = [];
  for (; it.valid(); it.next()) {
    const properties = new Map<string, FlamegraphPropertyDefinition>();
    for (const a of unagg) {
      const r = it.get(a.name);
      if (r !== null) {
        const value = r as string;
        properties.set(a.name, {
          displayName: a.displayName,
          value,
          isVisible: a.isVisible ? a.isVisible(value) : true,
          isAggregatable: false,
        });
      }
    }
    for (const a of agg) {
      const r = it.get(a.name);
      if (r !== null) {
        const value = r as string;
        properties.set(a.name, {
          displayName: a.displayName,
          value,
          isVisible: a.isVisible ? a.isVisible(value) : true,
          isAggregatable: true,
        });
      }
    }

    // Evaluate marker
    let marker: string | undefined;
    if (
      optionalMarker &&
      optionalMarker.isVisible(
        new Map([...properties].map(([k, v]) => [k, v.value])),
      )
    ) {
      marker = optionalMarker.name;
    }

    nodes.push({
      id: it.id,
      parentId: it.parentId,
      depth: it.depth,
      name: it.name,
      selfValue: it.selfValue,
      cumulativeValue: it.cumulativeValue,
      parentCumulativeValue: it.parentCumulativeValue ?? undefined,
      xStart: it.xStart,
      xEnd: it.xEnd,
      properties,
      marker,
    });
    if (it.depth === 1) {
      postiveRootsValue += it.cumulativeValue;
    } else if (it.depth === -1) {
      negativeRootsValue += it.cumulativeValue;
    }
    minDepth = Math.min(minDepth, it.depth);
    maxDepth = Math.max(maxDepth, it.depth);
  }
  const sumQuery = await engine.query(
    `select sum(value) v from _flamegraph_source_${uuid}`,
  );
  const unfilteredCumulativeValue = sumQuery.firstRow({v: NUM_NULL}).v ?? 0;
  return {
    nodes,
    allRootsCumulativeValue:
      view.kind === 'BOTTOM_UP' ? negativeRootsValue : postiveRootsValue,
    unfilteredCumulativeValue,
    minDepth,
    maxDepth,
    nodeActions,
    rootActions,
  };
}

