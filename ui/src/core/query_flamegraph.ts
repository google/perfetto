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

import {AsyncDisposableStack} from '../base/disposable';
import {Engine} from '../trace_processor/engine';
import {NUM, STR} from '../trace_processor/query_result';
import {createPerfettoTable} from '../trace_processor/sql_utils';
import {
  Flamegraph,
  FlamegraphFilters,
  FlamegraphQueryData,
} from '../widgets/flamegraph';
import {AsyncLimiter} from '../base/async_limiter';
import {assertExists} from '../base/logging';
import {Monitor} from '../base/monitor';
import {featureFlags} from './feature_flags';

interface QueryFlamegraphMetric {
  // The human readable name of the metric: will be shown to the user to change
  // between metrics.
  readonly name: string;

  // The human readable SI-style unit of `selfValue`. Values will be shown to the
  // user suffixed with this.
  readonly unit: string;

  // SQL statement which need to be run in preparation for being able to execute
  // `statement`.
  readonly dependencySql?: string;

  // A single SQL statement which returns the columns `id`, `parentId`, `name` and
  // `selfValue`
  readonly statement: string;
}

// Given a table and columns on those table (corresponding to metrics),
// returns an array of `QueryFlamegraphMetric` structs which can be passed
// in QueryFlamegraph's attrs.
//
// `tableOrSubquery` should have the columns `id`, `parentId`, `name` and all
// columns specified by `tableMetrics[].name`.
export function metricsFromTableOrSubquery(
  tableOrSubquery: string,
  tableMetrics: ReadonlyArray<{name: string; unit: string; columnName: string}>,
  dependencySql?: string,
): QueryFlamegraphMetric[] {
  const metrics = [];
  for (const {name, unit, columnName} of tableMetrics) {
    metrics.push({
      name,
      unit,
      dependencySql,
      statement: `
        select id, parentId, name, ${columnName} as value
        from ${tableOrSubquery}
      `,
    });
  }
  return metrics;
}

export interface QueryFlamegraphAttrs {
  readonly engine: Engine;
  readonly metrics: ReadonlyArray<QueryFlamegraphMetric>;
}

// A Mithril component which wraps the `Flamegraph` widget and fetches the data for the
// widget by querying an `Engine`.
export class QueryFlamegraph implements m.ClassComponent<QueryFlamegraphAttrs> {
  private selectedMetricName;
  private data?: FlamegraphQueryData;
  private filters: FlamegraphFilters = {
    showStack: [],
    hideStack: [],
    showFrame: [],
    hideFrame: [],
  };
  private attrs: QueryFlamegraphAttrs;
  private selMonitor = new Monitor([() => this.attrs.metrics]);
  private queryLimiter = new AsyncLimiter();

  constructor({attrs}: m.Vnode<QueryFlamegraphAttrs>) {
    this.attrs = attrs;
    this.selectedMetricName = attrs.metrics[0].name;
  }

  view({attrs}: m.Vnode<QueryFlamegraphAttrs>) {
    this.attrs = attrs;
    if (this.selMonitor.ifStateChanged()) {
      this.selectedMetricName = attrs.metrics[0].name;
      this.data = undefined;
      this.fetchData(attrs);
    }
    return m(Flamegraph, {
      metrics: attrs.metrics,
      selectedMetricName: this.selectedMetricName,
      data: this.data,
      onMetricChange: (name) => {
        this.selectedMetricName = name;
        this.data = undefined;
        this.fetchData(attrs);
      },
      onFiltersChanged: (filters) => {
        this.filters = filters;
        this.data = undefined;
        this.fetchData(attrs);
      },
    });
  }

  private async fetchData(attrs: QueryFlamegraphAttrs) {
    const {statement, dependencySql} = assertExists(
      attrs.metrics.find((metric) => metric.name === this.selectedMetricName),
    );
    const engine = attrs.engine;
    const filters = this.filters;
    this.queryLimiter.schedule(async () => {
      this.data = await computeFlamegraphTree(
        engine,
        dependencySql,
        statement,
        filters,
      );
    });
  }
}

async function computeFlamegraphTree(
  engine: Engine,
  dependencySql: string | undefined,
  sql: string,
  {
    showStack,
    hideStack,
    showFrame,
    hideFrame,
  }: {
    readonly showStack: ReadonlyArray<string>;
    readonly hideStack: ReadonlyArray<string>;
    readonly showFrame: ReadonlyArray<string>;
    readonly hideFrame: ReadonlyArray<string>;
  },
) {
  const allStackBits = (1 << showStack.length) - 1;
  const showStackFilter =
    showStack.length === 0
      ? '0'
      : showStack.map((x, i) => `((name like '%${x}%') << ${i})`).join(' | ');
  const hideStackFilter =
    hideStack.length === 0
      ? 'false'
      : hideStack.map((x) => `name like '%${x}%'`).join(' OR ');
  const showFrameFilter =
    showFrame.length === 0
      ? 'true'
      : showFrame.map((x) => `name like '%${x}%'`).join(' OR ');
  const hideFrameFilter =
    hideFrame.length === 0
      ? 'false'
      : hideFrame.map((x) => `name like '%${x}%'`).join(' OR ');

  if (dependencySql !== undefined) {
    await engine.query(dependencySql);
  }
  await engine.query(`include perfetto module viz.flamegraph;`);

  const disposable = new AsyncDisposableStack();
  try {
    disposable.use(
      await createPerfettoTable(
        engine,
        '_flamegraph_source',
        `
        select *
        from _viz_flamegraph_prepare_filter!(
          (${sql}),
          (${showFrameFilter}),
          (${hideFrameFilter}),
          (${showStackFilter}),
          (${hideStackFilter}),
          ${1 << showStack.length}
        )
      `,
      ),
    );
    disposable.use(
      await createPerfettoTable(
        engine,
        '_flamegraph_raw_top_down',
        `select * from _viz_flamegraph_filter_and_hash!(_flamegraph_source)`,
      ),
    );
    disposable.use(
      await createPerfettoTable(
        engine,
        '_flamegraph_top_down',
        `
        select * from _viz_flamegraph_merge_hashes!(
          _flamegraph_raw_top_down,
          _flamegraph_source
        )
      `,
      ),
    );
    disposable.use(
      await createPerfettoTable(
        engine,
        '_flamegraph_raw_bottom_up',
        `
        select *
        from _viz_flamegraph_accumulate!(_flamegraph_top_down, ${allStackBits})
      `,
      ),
    );
    disposable.use(
      await createPerfettoTable(
        engine,
        '_flamegraph_windowed',
        `
        select *
        from _viz_flamegraph_local_layout!(
          _flamegraph_raw_bottom_up,
          _flamegraph_top_down
        );
      `,
      ),
    );
    const res = await engine.query(`
      select *
      from _viz_flamegraph_global_layout!(
        _flamegraph_windowed,
        _flamegraph_raw_bottom_up,
        _flamegraph_top_down
      )
    `);
    const it = res.iter({
      id: NUM,
      parentId: NUM,
      depth: NUM,
      name: STR,
      selfValue: NUM,
      cumulativeValue: NUM,
      xStart: NUM,
      xEnd: NUM,
    });
    let allRootsCumulativeValue = 0;
    let maxDepth = 0;
    const nodes = [];
    for (; it.valid(); it.next()) {
      nodes.push({
        id: it.id,
        parentId: it.parentId,
        depth: it.depth,
        name: it.name,
        selfValue: it.selfValue,
        cumulativeValue: it.cumulativeValue,
        xStart: it.xStart,
        xEnd: it.xEnd,
      });
      if (it.parentId === -1) {
        allRootsCumulativeValue += it.cumulativeValue;
      }
      maxDepth = Math.max(maxDepth, it.depth);
    }
    return {nodes, allRootsCumulativeValue, maxDepth};
  } finally {
    await disposable.disposeAsync();
  }
}

export const USE_NEW_FLAMEGRAPH_IMPL = featureFlags.register({
  id: 'useNewFlamegraphImpl',
  name: 'Use new flamegraph implementation',
  description: 'Use new flamgraph implementation in details panels.',
  defaultValue: false,
});
