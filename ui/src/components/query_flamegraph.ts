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
import {
  createPerfettoIndex,
  createPerfettoTable,
} from '../trace_processor/sql_utils';
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
  FlamegraphView,
  FlamegraphOptionalAction,
  FlamegraphOptionalMarker,
} from '../widgets/flamegraph';
import {Trace} from '../public/trace';
import {sqliteString} from '../base/string_utils';
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
  optionalActions?: ReadonlyArray<FlamegraphOptionalAction>,
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
      optionalActions,
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

  const agg = aggregatableProperties ?? [];
  const aggCols = agg.map((x) => x.name);
  const unagg = unaggregatableProperties ?? [];
  const unaggCols = unagg.map((x) => x.name);

  const matchingColumns = ['name', ...unaggCols];
  const matchExpr = (x: string) =>
    matchingColumns.map(
      (c) =>
        `(IFNULL(${c}, '') like ${sqliteString(makeSqlFilter(x))} escape '\\')`,
    );

  const showStackFilter =
    showStackAndPivot.length === 0
      ? '0'
      : showStackAndPivot
          .map((x, i) => `((${matchExpr(x).join(' OR ')}) << ${i})`)
          .join(' | ');
  const showStackBits = (1 << showStackAndPivot.length) - 1;

  const hideStackFilter =
    hideStack.length === 0
      ? 'false'
      : hideStack
          .map((x) => matchExpr(x))
          .flat()
          .join(' OR ');

  const showFromFrameFilter =
    showFromFrame.length === 0
      ? '0'
      : showFromFrame
          .map((x, i) => `((${matchExpr(x).join(' OR ')}) << ${i})`)
          .join(' | ');
  const showFromFrameBits = (1 << showFromFrame.length) - 1;

  const hideFrameFilter =
    hideFrame.length === 0
      ? 'false'
      : hideFrame
          .map((x) => matchExpr(x))
          .flat()
          .join(' OR ');

  const pivotFilter = getPivotFilter(view, matchExpr);

  const nodeActions = optionalNodeActions ?? [];
  const rootActions = optionalRootActions ?? [];

  const groupingColumns = `(${(unaggCols.length === 0 ? ['groupingColumn'] : unaggCols).join()})`;
  const groupedColumns = `(${(aggCols.length === 0 ? ['groupedColumn'] : aggCols).join()})`;

  if (dependencySql !== undefined) {
    await engine.query(dependencySql);
  }
  await engine.query(`include perfetto module viz.flamegraph;`);

  const uuid = uuidv4Sql();
  await using disposable = new AsyncDisposableStack();

  disposable.use(
    await createPerfettoTable({
      engine,
      name: `_flamegraph_materialized_statement_${uuid}`,
      as: statement,
    }),
  );
  disposable.use(
    await createPerfettoIndex({
      engine,
      name: `_flamegraph_materialized_statement_${uuid}_index`,
      on: `_flamegraph_materialized_statement_${uuid}(parentId)`,
    }),
  );

  // TODO(lalitm): this doesn't need to be called unless we have
  // a non-empty set of filters.
  disposable.use(
    await createPerfettoTable({
      engine,
      name: `_flamegraph_source_${uuid}`,
      as: `
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
    }),
  );
  // TODO(lalitm): this doesn't need to be called unless we have
  // a non-empty set of filters.
  disposable.use(
    await createPerfettoTable({
      engine,
      name: `_flamegraph_filtered_${uuid}`,
      as: `
        select *
        from _viz_flamegraph_filter_frames!(
          _flamegraph_source_${uuid},
          ${showFromFrameBits}
        )
      `,
    }),
  );
  disposable.use(
    await createPerfettoIndex({
      engine,
      name: `_flamegraph_filtered_${uuid}_index`,
      on: `_flamegraph_filtered_${uuid}(parentId)`,
    }),
  );
  disposable.use(
    await createPerfettoTable({
      engine,
      name: `_flamegraph_accumulated_${uuid}`,
      as: `
        select *
        from _viz_flamegraph_accumulate!(
          _flamegraph_filtered_${uuid},
          ${showStackBits}
        )
      `,
    }),
  );
  disposable.use(
    await createPerfettoTable({
      engine,
      name: `_flamegraph_hash_${uuid}`,
      as: `
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
    }),
  );
  disposable.use(
    await createPerfettoTable({
      engine,
      name: `_flamegraph_merged_${uuid}`,
      as: `
        select *
        from _viz_flamegraph_merge_hashes!(
          _flamegraph_hash_${uuid},
          ${groupingColumns},
          ${computeGroupedAggExprs(agg)}
        )
      `,
    }),
  );
  disposable.use(
    await createPerfettoIndex({
      engine,
      name: `_flamegraph_merged_${uuid}_index`,
      on: `_flamegraph_merged_${uuid}(parentId)`,
    }),
  );
  disposable.use(
    await createPerfettoTable({
      engine,
      name: `_flamegraph_layout_${uuid}`,
      as: `
        select *
        from _viz_flamegraph_local_layout!(
          _flamegraph_merged_${uuid}
        );
      `,
    }),
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
    const properties = new Map<string, FlamegraphPropertyDefinition>();
    for (const a of [...agg, ...unagg]) {
      const r = it.get(a.name);
      if (r !== null) {
        const value = r as string;
        properties.set(a.name, {
          displayName: a.displayName,
          value,
          isVisible: a.isVisible ? a.isVisible(value) : true,
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

function makeSqlFilter(x: string) {
  if (x.startsWith('^') && x.endsWith('$')) {
    return x.slice(1, -1);
  }
  return `%${x}%`;
}

function getPivotFilter(
  view: FlamegraphView,
  makeFilterExpr: (x: string) => string[],
) {
  if (view.kind === 'PIVOT') {
    return makeFilterExpr(view.pivot).join(' OR ');
  }
  if (view.kind === 'BOTTOM_UP') {
    return 'value > 0';
  }
  return '0';
}

function computeGroupedAggExprs(agg: ReadonlyArray<AggQueryFlamegraphColumn>) {
  const aggFor = (x: AggQueryFlamegraphColumn) => {
    switch (x.mergeAggregation) {
      case 'ONE_OR_SUMMARY':
        return `
          ${x.name} || IIF(
            COUNT(DISTINCT ${x.name}) = 1,
            '',
            ' ' || ' and ' || cast_string!(COUNT(DISTINCT ${x.name})) || ' others'
          ) AS ${x.name}
        `;
      case 'SUM':
        return `SUM(${x.name}) AS ${x.name}`;
      case 'CONCAT_WITH_COMMA':
        return `GROUP_CONCAT(${x.name}, ',') AS ${x.name}`;
    }
  };
  return `(${agg.length === 0 ? 'groupedColumn' : agg.map((x) => aggFor(x)).join(',')})`;
}
