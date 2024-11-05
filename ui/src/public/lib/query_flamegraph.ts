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
import {AsyncLimiter} from '../../base/async_limiter';
import {AsyncDisposableStack} from '../../base/disposable_stack';
import {assertExists} from '../../base/logging';
import {Monitor} from '../../base/monitor';
import {uuidv4Sql} from '../../base/uuid';
import {Engine} from '../../trace_processor/engine';
import {
  createPerfettoIndex,
  createPerfettoTable,
} from '../../trace_processor/sql_utils';
import {
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
  UNKNOWN,
} from '../../trace_processor/query_result';
import {
  Flamegraph,
  FlamegraphQueryData,
  FlamegraphState,
  FlamegraphView,
} from '../../widgets/flamegraph';
import {Trace} from '../trace';

export interface QueryFlamegraphColumn {
  // The name of the column in SQL.
  readonly name: string;

  // The human readable name describing the contents of the column.
  readonly displayName: string;
}

export interface AggQueryFlamegraphColumn extends QueryFlamegraphColumn {
  // The aggregation to be run when nodes are merged together in the flamegraph.
  //
  // TODO(lalitm): consider adding extra functions here (e.g. a top 5 or similar).
  readonly mergeAggregation: 'ONE_OR_NULL' | 'SUM';
}

export interface QueryFlamegraphMetric {
  // The human readable name of the metric: will be shown to the user to change
  // between metrics.
  readonly name: string;

  // The human readable SI-style unit of `selfValue`. Values will be shown to
  // the user suffixed with this.
  readonly unit: string;

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
}

export interface QueryFlamegraphState {
  state: FlamegraphState;
}

// Given a table and columns on those table (corresponding to metrics),
// returns an array of `QueryFlamegraphMetric` structs which can be passed
// in QueryFlamegraph's attrs.
//
// `tableOrSubquery` should have the columns `id`, `parentId`, `name` and all
// columns specified by `tableMetrics[].name`, `unaggregatableProperties` and
// `aggregatableProperties`.
export function metricsFromTableOrSubquery(
  tableOrSubquery: string,
  tableMetrics: ReadonlyArray<{name: string; unit: string; columnName: string}>,
  dependencySql?: string,
  unaggregatableProperties?: ReadonlyArray<QueryFlamegraphColumn>,
  aggregatableProperties?: ReadonlyArray<AggQueryFlamegraphColumn>,
): QueryFlamegraphMetric[] {
  const metrics = [];
  for (const {name, unit, columnName} of tableMetrics) {
    metrics.push({
      name,
      unit,
      dependencySql,
      statement: `
        select *, ${columnName} as value
        from ${tableOrSubquery}
      `,
      unaggregatableProperties,
      aggregatableProperties,
    });
  }
  return metrics;
}

// A Perfetto UI component which wraps the `Flamegraph` widget and fetches the
// data for the widget by querying an `Engine`.
export class QueryFlamegraph {
  private data?: FlamegraphQueryData;
  private readonly selMonitor = new Monitor([() => this.state.state]);
  private readonly queryLimiter = new AsyncLimiter();

  constructor(
    private readonly trace: Trace,
    private readonly metrics: ReadonlyArray<QueryFlamegraphMetric>,
    private state: QueryFlamegraphState,
  ) {}

  render() {
    if (this.selMonitor.ifStateChanged()) {
      const metric = assertExists(
        this.metrics.find(
          (x) => this.state.state.selectedMetricName === x.name,
        ),
      );
      const engine = this.trace.engine;
      const state = this.state;
      this.data = undefined;
      this.queryLimiter.schedule(async () => {
        this.data = undefined;
        this.data = await computeFlamegraphTree(engine, metric, state.state);
      });
    }
    return m(Flamegraph, {
      metrics: this.metrics,
      data: this.data,
      state: this.state.state,
      onStateChange: (state) => {
        this.state.state = state;
        this.trace.scheduleFullRedraw();
      },
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
  }: QueryFlamegraphMetric,
  {filters, view}: FlamegraphState,
): Promise<FlamegraphQueryData> {
  const showStack = filters
    .filter((x) => x.kind === 'SHOW_STACK')
    .map((x) => x.filter);
  const hideStack = filters
    .filter((x) => x.kind === 'HIDE_STACK')
    .map((x) => x.filter);
  const showFromFrame = filters
    .filter((x) => x.kind === 'SHOW_FROM_FRAME')
    .map((x) => x.filter);
  const hideFrame = filters
    .filter((x) => x.kind === 'HIDE_FRAME')
    .map((x) => x.filter);

  // Pivot also essentially acts as a "show stack" filter so treat it like one.
  const showStackAndPivot = [...showStack];
  if (view.kind === 'PIVOT') {
    showStackAndPivot.push(view.pivot);
  }

  const showStackFilter =
    showStackAndPivot.length === 0
      ? '0'
      : showStackAndPivot
          .map(
            (x, i) => `((name like '${makeSqlFilter(x)}' escape '\\') << ${i})`,
          )
          .join(' | ');
  const showStackBits = (1 << showStackAndPivot.length) - 1;

  const hideStackFilter =
    hideStack.length === 0
      ? 'false'
      : hideStack
          .map((x) => `name like '${makeSqlFilter(x)}' escape '\\'`)
          .join(' OR ');

  const showFromFrameFilter =
    showFromFrame.length === 0
      ? '0'
      : showFromFrame
          .map(
            (x, i) => `((name like '${makeSqlFilter(x)}' escape '\\') << ${i})`,
          )
          .join(' | ');
  const showFromFrameBits = (1 << showFromFrame.length) - 1;

  const hideFrameFilter =
    hideFrame.length === 0
      ? 'false'
      : hideFrame
          .map((x) => `name like '${makeSqlFilter(x)}' escape '\\'`)
          .join(' OR ');

  const pivotFilter = getPivotFilter(view);

  const unagg = unaggregatableProperties ?? [];
  const unaggCols = unagg.map((x) => x.name);

  const agg = aggregatableProperties ?? [];
  const aggCols = agg.map((x) => x.name);

  const groupingColumns = `(${(unaggCols.length === 0 ? ['groupingColumn'] : unaggCols).join()})`;
  const groupedColumns = `(${(aggCols.length === 0 ? ['groupedColumn'] : aggCols).join()})`;

  if (dependencySql !== undefined) {
    await engine.query(dependencySql);
  }
  await engine.query(`include perfetto module viz.flamegraph;`);

  const uuid = uuidv4Sql();
  await using disposable = new AsyncDisposableStack();

  disposable.use(
    await createPerfettoTable(
      engine,
      `_flamegraph_materialized_statement_${uuid}`,
      statement,
    ),
  );
  disposable.use(
    await createPerfettoIndex(
      engine,
      `_flamegraph_materialized_statement_${uuid}_index`,
      `_flamegraph_materialized_statement_${uuid}(parentId)`,
    ),
  );

  // TODO(lalitm): this doesn't need to be called unless we have
  // a non-empty set of filters.
  disposable.use(
    await createPerfettoTable(
      engine,
      `_flamegraph_source_${uuid}`,
      `
        select *
        from _viz_flamegraph_prepare_filter!(
          (
            select
              s.id,
              s.parentId,
              s.name,
              s.value,
              ${(unaggCols.length === 0
                ? [`'' as groupingColumn`]
                : unaggCols.map((x) => `s.${x}`)
              ).join()},
              ${(aggCols.length === 0
                ? [`'' as groupedColumn`]
                : aggCols.map((x) => `s.${x}`)
              ).join()}
            from _flamegraph_materialized_statement_${uuid} s
          ),
          (${showStackFilter}),
          (${hideStackFilter}),
          (${showFromFrameFilter}),
          (${hideFrameFilter}),
          (${pivotFilter}),
          ${1 << showStackAndPivot.length},
          ${groupingColumns}
        )
      `,
    ),
  );
  // TODO(lalitm): this doesn't need to be called unless we have
  // a non-empty set of filters.
  disposable.use(
    await createPerfettoTable(
      engine,
      `_flamegraph_filtered_${uuid}`,
      `
        select *
        from _viz_flamegraph_filter_frames!(
          _flamegraph_source_${uuid},
          ${showFromFrameBits}
        )
      `,
    ),
  );
  disposable.use(
    await createPerfettoTable(
      engine,
      `_flamegraph_accumulated_${uuid}`,
      `
        select *
        from _viz_flamegraph_accumulate!(
          _flamegraph_filtered_${uuid},
          ${showStackBits}
        )
      `,
    ),
  );
  disposable.use(
    await createPerfettoTable(
      engine,
      `_flamegraph_hash_${uuid}`,
      `
        select *
        from _viz_flamegraph_downwards_hash!(
          _flamegraph_source_${uuid},
          _flamegraph_filtered_${uuid},
          _flamegraph_accumulated_${uuid},
          ${groupingColumns},
          ${groupedColumns},
          ${view.kind === 'BOTTOM_UP' ? 'FALSE' : 'TRUE'}
        )
        union all
        select *
        from _viz_flamegraph_upwards_hash!(
          _flamegraph_source_${uuid},
          _flamegraph_filtered_${uuid},
          _flamegraph_accumulated_${uuid},
          ${groupingColumns},
          ${groupedColumns}
        )
        order by hash
      `,
    ),
  );
  disposable.use(
    await createPerfettoTable(
      engine,
      `_flamegraph_merged_${uuid}`,
      `
        select *
        from _viz_flamegraph_merge_hashes!(
          _flamegraph_hash_${uuid},
          ${groupingColumns},
          ${computeGroupedAggExprs(agg)}
        )
      `,
    ),
  );
  disposable.use(
    await createPerfettoTable(
      engine,
      `_flamegraph_layout_${uuid}`,
      `
        select *
        from _viz_flamegraph_local_layout!(
          _flamegraph_merged_${uuid}
        );
      `,
    ),
  );
  const res = await engine.query(`
    select *
    from _viz_flamegraph_global_layout!(
      _flamegraph_merged_${uuid},
      _flamegraph_layout_${uuid},
      ${groupingColumns},
      ${groupedColumns}
    )
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
    const properties = new Map<string, string>();
    for (const a of [...agg, ...unagg]) {
      const r = it.get(a.name);
      if (r !== null) {
        properties.set(a.displayName, r as string);
      }
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
  };
}

function makeSqlFilter(x: string) {
  if (x.startsWith('^') && x.endsWith('$')) {
    return x.slice(1, -1);
  }
  return `%${x}%`;
}

function getPivotFilter(view: FlamegraphView) {
  if (view.kind === 'PIVOT') {
    return `name like '${makeSqlFilter(view.pivot)}'`;
  }
  if (view.kind === 'BOTTOM_UP') {
    return 'value > 0';
  }
  return '0';
}

function computeGroupedAggExprs(agg: ReadonlyArray<AggQueryFlamegraphColumn>) {
  const aggFor = (x: AggQueryFlamegraphColumn) => {
    switch (x.mergeAggregation) {
      case 'ONE_OR_NULL':
        return `IIF(COUNT() = 1, ${x.name}, NULL) AS ${x.name}`;
      case 'SUM':
        return `SUM(${x.name}) AS ${x.name}`;
    }
  };
  return `(${agg.length === 0 ? 'groupedColumn' : agg.map((x) => aggFor(x)).join(',')})`;
}
