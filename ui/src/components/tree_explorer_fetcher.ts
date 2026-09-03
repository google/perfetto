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

import {AsyncDisposableStack} from '../base/disposable_stack';
import {ensureExists} from '../base/assert';
import type {Engine} from '../trace_processor/engine';
import {
  createVirtualTable,
  type DisposableSqlEntity,
} from '../trace_processor/sql_utils';
import {
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
  UNKNOWN,
} from '../trace_processor/query_result';
import type {
  TreeExplorerData,
  TreeExplorerOptionalAction,
  TreeExplorerOptionalMarker,
  TreeExplorerPropertyDefinition,
  TreeExplorerState,
} from '../widgets/tree_explorer';
import {metricId} from '../widgets/tree_explorer';
import type {Trace} from '../public/trace';
import {sqliteString} from '../base/string_utils';
import {parseUserFilterRegex} from '../widgets/flamegraph_regex';
import type {SharedAsyncDisposable} from '../base/shared_disposable';

export interface TreeExplorerQueryColumn {
  // The name of the column in SQL.
  readonly name: string;

  // The human readable name describing the contents of the column.
  readonly displayName: string;

  // Function that determines whether the property should be displayed for a
  // given node.
  readonly isVisible?: (value: string) => boolean;
}

export interface AggTreeExplorerQueryColumn extends TreeExplorerQueryColumn {
  // The aggregation to be run when nodes are merged together in the tree.
  readonly mergeAggregation: 'ONE_OR_SUMMARY' | 'SUM' | 'CONCAT_WITH_COMMA';
}

export interface TreeExplorerQueryMetric {
  // Stable identity used in persisted state. Defaults to `name`.
  readonly id?: string;

  // The human readable name of the metric: will be shown to the user to change
  // between metrics.
  readonly name: string;

  // The human readable SI-style unit of `selfValue`. Values will be shown to
  // the user suffixed with this.
  readonly unit: string;

  // Where the measure came from. Undefined is treated as ADDED.
  readonly provenance?: 'DEFAULT' | 'ADDED';

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
  readonly unaggregatableProperties?: ReadonlyArray<TreeExplorerQueryColumn>;

  // Additional contextual columns containing data which will be displayed to
  // the user if there is no merging. If there is merging, currently the value
  // will not be shown.
  //
  // Examples include the source file and line number.
  readonly aggregatableProperties?: ReadonlyArray<AggTreeExplorerQueryColumn>;

  // Optional actions to be taken on the tree nodes. Accessible from the
  // flamegraph tooltip.
  //
  // Examples include showing a table of objects from a class reference
  // hierarchy.
  readonly optionalNodeActions?: ReadonlyArray<TreeExplorerOptionalAction>;

  // Optional actions to be taken on the tree root. Accessible from the
  // flamegraph tooltip.
  //
  // Examples include showing a table of objects from a class reference
  // hierarchy.
  readonly optionalRootActions?: ReadonlyArray<TreeExplorerOptionalAction>;

  // Optional marker to be displayed on nodes. Marker appears as a visual
  // indicator (small dot) on the left side of flamegraph nodes and is shown
  // in the tooltip.
  //
  // Examples include marking inlined functions, optimized code, etc.
  readonly optionalMarker?: TreeExplorerOptionalMarker;
}

export interface MetricsFromTableOrSubqueryOptions {
  readonly tableOrSubquery: string;
  readonly tableMetrics: ReadonlyArray<{
    id?: string;
    name: string;
    unit: string;
    columnName: string;
    provenance?: 'DEFAULT' | 'ADDED';
  }>;
  readonly dependencySql?: string;
  readonly unaggregatableProperties?: ReadonlyArray<TreeExplorerQueryColumn>;
  readonly aggregatableProperties?: ReadonlyArray<AggTreeExplorerQueryColumn>;
  readonly optionalActions?: ReadonlyArray<TreeExplorerOptionalAction>;
  readonly nameColumnLabel?: string;
}

// Given a table and columns on those table (corresponding to metrics),
// returns an array of `TreeExplorerQueryMetric` structs which can be passed
// in TreeExplorerPanel's attrs.
//
// `tableOrSubquery` should have the columns `id`, `parentId`, `name` and all
// columns specified by `tableMetrics[].name`, `unaggregatableProperties` and
// `aggregatableProperties`.
export function metricsFromTableOrSubquery(
  opts: MetricsFromTableOrSubqueryOptions,
): TreeExplorerQueryMetric[] {
  const metrics = [];
  for (const {id, name, unit, columnName, provenance} of opts.tableMetrics) {
    metrics.push({
      id,
      name,
      unit,
      provenance,
      nameColumnLabel: opts.nameColumnLabel,
      dependencySql: opts.dependencySql,
      statement: `
        select *, ${columnName} as value
        from ${opts.tableOrSubquery}
      `,
      unaggregatableProperties: opts.unaggregatableProperties,
      aggregatableProperties: opts.aggregatableProperties,
      optionalNodeActions: opts.optionalActions,
    });
  }
  return metrics;
}

interface MetricTable {
  readonly metric: TreeExplorerQueryMetric;
  readonly table: DisposableSqlEntity;
  readonly unfilteredCumulativeValue: number;
}

export type TreeExplorerFetcherDependency =
  SharedAsyncDisposable<AsyncDisposable>;

// Fetches tree explorer data by querying an `Engine`: turns a
// (metric, state) pair into the filtered tree the views display. Purely a
// data-layer object with no rendering; TreeExplorerPanel drives it.
export class TreeExplorerFetcher implements AsyncDisposable {
  private readonly dependencies: ReadonlyArray<
    SharedAsyncDisposable<AsyncDisposable>
  >;
  private readonly metricTables: MetricTable[] = [];

  constructor(
    private readonly trace: Trace,
    dependencies: ReadonlyArray<TreeExplorerFetcherDependency> = [],
  ) {
    this.dependencies = dependencies.map((d) => d.clone());
  }

  async [Symbol.asyncDispose](): Promise<void> {
    for (const entry of this.metricTables) {
      await entry.table[Symbol.asyncDispose]();
    }
    for (const dependency of this.dependencies ?? []) {
      await dependency[Symbol.asyncDispose]?.();
    }
  }

  // Fetches the tree for the metric selected in `state`, with all of
  // `state`'s filters and view applied. Returns undefined if the fetcher's
  // dependencies were disposed while the fetch was in flight.
  async fetch(
    metrics: ReadonlyArray<TreeExplorerQueryMetric>,
    state: TreeExplorerState,
  ): Promise<TreeExplorerData | undefined> {
    const metric = ensureExists(
      metrics.find((x) => state.selectedMetricId === metricId(x)),
    );
    // Clone all dependencies so they cannot be dropped while this function
    // is running. Disposing these clones after the function returns does not
    // drop the tables while either this instance or the caller still owns a
    // clone.
    await using trash = new AsyncDisposableStack();
    for (const dependency of this.dependencies ?? []) {
      // If the dependency is disposed, it means that we have already ended
      // up cleaning up the object so none of this matters. Just return.
      if (dependency.isDisposed) {
        return undefined;
      }
      trash.use(dependency.clone());
    }
    const table = await this.getMetricTable(metric);
    return await computeTree(this.trace.engine, table, state);
  }

  private async getMetricTable(
    metric: TreeExplorerQueryMetric,
  ): Promise<MetricTable> {
    const cached = this.metricTables.find((entry) => entry.metric === metric);
    if (cached) {
      return cached;
    }
    if (metric.dependencySql !== undefined) {
      await this.trace.engine.query(metric.dependencySql);
    }
    const properties = [
      ...(metric.unaggregatableProperties ?? []),
      ...(metric.aggregatableProperties ?? []),
    ];
    const sourceColumns = [
      's.id',
      's.parentId as parent_id',
      's.name',
      's.value',
      ...properties.map((property) => `s.${property.name}`),
    ];
    const table = await createVirtualTable({
      engine: this.trace.engine,
      using: `__intrinsic_flamegraph((
        select ${sourceColumns.join(', ')}
        from (${metric.statement}) s
      ))`,
    });
    try {
      const result = await this.trace.engine.query(`
        select cumulative_value
        from ${table.name}(__intrinsic_flamegraph_config(
          'value', 'value',
          'view', 'TOP_DOWN'
        ))
        where __intrinsic_flamegraph_find(_tree_id, 'SUPER_ROOT')
      `);
      const entry = {
        metric,
        table,
        unfilteredCumulativeValue: result.firstRow({
          cumulative_value: NUM,
        }).cumulative_value,
      };
      this.metricTables.push(entry);
      return entry;
    } catch (error) {
      await table[Symbol.asyncDispose]();
      throw error;
    }
  }
}

async function computeTree(
  engine: Engine,
  metricTable: MetricTable,
  {filters, view}: TreeExplorerState,
): Promise<TreeExplorerData> {
  const {
    unaggregatableProperties,
    aggregatableProperties,
    optionalNodeActions,
    optionalRootActions,
    optionalMarker,
  } = metricTable.metric;
  const agg = aggregatableProperties ?? [];
  const aggCols = agg.map((x) => x.name);
  const unagg = unaggregatableProperties ?? [];
  const unaggCols = unagg.map((x) => x.name);
  const nodeActions = optionalNodeActions ?? [];
  const rootActions = optionalRootActions ?? [];

  // Convert the UI syntax into finished patterns and explicit flags before
  // passing them to the operator.
  const configArgs = [`'view', ${sqliteString(view.kind)}`, `'value', 'value'`];
  if (view.kind === 'PIVOT' || view.kind === 'FROM_FRAME') {
    const filter = view.kind === 'PIVOT' ? view.pivot : view.pattern;
    const regex = parseUserFilterRegex(filter);
    configArgs.push(
      `'view_pattern', ${sqliteString(regex.pattern)}, ` +
        sqliteString(regex.flags),
    );
  }
  for (const filter of filters) {
    if (filter.kind !== 'OPTIONS') {
      const regex = parseUserFilterRegex(filter.filter);
      configArgs.push(
        `'filter', ${sqliteString(filter.kind)}, ` +
          `${sqliteString(regex.pattern)}, ${sqliteString(regex.flags)}`,
      );
    }
  }
  for (const column of unaggCols) {
    configArgs.push(`'grouping', ${sqliteString(column)}`);
  }
  for (const column of agg) {
    configArgs.push(
      `'aggregate', ${sqliteString(column.mergeAggregation)}, ` +
        `${sqliteString(column.name)}, ${sqliteString(column.name)}`,
    );
  }

  const outputColumns = [
    '_tree_id as id',
    'IFNULL(_tree_parent_id, -1) as parentId',
    'depth',
    `IIF(IFNULL(name, '') = '', 'unknown', name) as name`,
    'self_value as selfValue',
    'cumulative_value as cumulativeValue',
    'parent_cumulative_value as parentCumulativeValue',
    'x_start as xStart',
    'x_end as xEnd',
    ...unaggCols,
    ...aggCols,
  ];

  // The operator emits rows pre-ordered for rendering with layout geometry
  // attached; nodes with no cumulative value are invisible and skipped.
  const res = await engine.query(`
    select ${outputColumns.join(', ')}
    from ${metricTable.table.name}(
      __intrinsic_flamegraph_config(${configArgs.join(', ')})
    )
    where cumulative_value > 0
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
    const properties = new Map<string, TreeExplorerPropertyDefinition>();
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
        // UNKNOWN-typed aggregations (e.g. SUM) can yield number/bigint.
        const value = String(r);
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
  return {
    nodes,
    allRootsCumulativeValue:
      view.kind === 'BOTTOM_UP' ? negativeRootsValue : postiveRootsValue,
    unfilteredCumulativeValue: metricTable.unfilteredCumulativeValue,
    minDepth,
    maxDepth,
    nodeActions,
    rootActions,
  };
}
