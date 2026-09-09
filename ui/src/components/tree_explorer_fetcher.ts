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

import {ensureExists} from '../base/assert';
import {
  AsyncMemo,
  AtomicTaskQueue,
  type AsyncMemoResult,
} from '../base/async_memo';
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

interface MetricTable extends AsyncDisposable {
  readonly metric: TreeExplorerQueryMetric;
  readonly table: DisposableSqlEntity;
  readonly unfilteredCumulativeValue: number;
}

// Fetches tree explorer data by querying an `Engine`: turns a
// (metric, state) pair into the filtered tree the views display. Purely a
// data-layer object with no rendering; TreeExplorerPanel drives it.
//
// All work (table creation, tree fetches, disposal) is scheduled on a single
// AtomicTaskQueue through AsyncMemo, so a metric's virtual table is only
// disposed after any in-flight query against it has completed.
export class TreeExplorerFetcher implements AsyncDisposable {
  // One memo per metric object we have seen. The map key *is* the identity:
  // a new metric object (same id, different SQL) gets a new memo and a new
  // table. `seq` is a stable numeric stand-in for the metric in the JSON
  // memo keys (metric objects are not JSON-serializable).
  private readonly tableMemos = new Map<
    TreeExplorerQueryMetric,
    {memo: AsyncMemo<MetricTable>; seq: number}
  >();
  private readonly dataMemo: AsyncMemo<TreeExplorerData>;
  private nextSeq = 0;

  constructor(
    private readonly trace: Trace,
    private readonly queue = new AtomicTaskQueue(),
  ) {
    this.dataMemo = new AsyncMemo<TreeExplorerData>(queue);
  }

  async [Symbol.asyncDispose](): Promise<void> {
    this.dataMemo.dispose();
    // Disposing a memo schedules its cache disposal through the shared
    // queue, i.e. after any in-flight work on it; the queue handles the
    // synchronization, we just ask every memo to go away.
    for (const {memo} of this.tableMemos.values()) {
      memo.dispose();
    }
  }

  // Call once per render: returns the tree currently available for the
  // metric selected in `state` and schedules any needed work. `state.view`
  // must already be the effective view (see TreeExplorerPanel).
  use(
    metrics: ReadonlyArray<TreeExplorerQueryMetric>,
    state: TreeExplorerState,
  ): AsyncMemoResult<TreeExplorerData> {
    const metric = ensureExists(
      metrics.find((x) => state.selectedMetricId === metricId(x)),
    );
    let entry = this.tableMemos.get(metric);
    if (entry === undefined) {
      entry = {
        memo: new AsyncMemo<MetricTable>(this.queue),
        seq: ++this.nextSeq,
      };
      this.tableMemos.set(metric, entry);
    }
    // The memo is dedicated to this metric object, so its key is constant:
    // the table is created once and kept for the fetcher's lifetime.
    const table = entry.memo.use({
      key: {},
      compute: () => this.createMetricTable(metric),
    }).data;

    if (!table) {
      return {isPending: true};
    }

    return this.dataMemo.use({
      key: {
        metricSeq: entry.seq,
        filters: state.filters,
        addedMetricIds: state.addedMetricIds,
        view: state.view,
      },
      compute: () => computeTree(this.trace.engine, table!, state),
    });
  }

  private async createMetricTable(
    metric: TreeExplorerQueryMetric,
  ): Promise<MetricTable> {
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
      return {
        metric,
        table,
        unfilteredCumulativeValue: result.firstRow({
          cumulative_value: NUM,
        }).cumulative_value,
        [Symbol.asyncDispose]: () => table[Symbol.asyncDispose](),
      };
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
