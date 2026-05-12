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

import m from 'mithril';
import type {Trace} from '../../../public/trace';
import {time} from '../../../base/time';
import {QueryFlamegraphMetric} from '../../../components/query_flamegraph';
import {FlamegraphPanel} from '../../../components/flamegraph_panel';
import {
  Flamegraph,
  FlamegraphState,
  FlamegraphOptionalAction,
} from '../../../widgets/flamegraph';

// Referenced by session.openFlamegraphPivotedAt.
export const METRIC_OBJECT_SIZE = 'Object Size';
export const METRIC_DOMINATED_OBJECT_SIZE = 'Dominated Object Size';

// Same-trace baseline: dump (upid, ts) within the same engine. The diff
// SQL JOINs the current and baseline class trees on `path_hash_stable`
// (computed by hashing parent path + type_id + heap_type — stable across
// dumps in a single trace because class ids are trace-global).
export interface FlamegraphBaselineRef {
  readonly upid: number;
  readonly ts: time;
}

interface FlamegraphViewAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly ts: time;
  readonly state: FlamegraphState | undefined;
  readonly onStateChange: (state: FlamegraphState) => void;
  // Open the flamegraph-objects tab for `pathHashes` (CSV).
  readonly onShowObjects: (pathHashes: string, isDominator: boolean) => void;
  // When set, build diff metrics that color nodes by delta direction and
  // size them by |delta|. Only same-trace baselines are supported here —
  // cross-engine SQL JOINs aren't possible across separate workers.
  readonly baseline?: FlamegraphBaselineRef;
}

// path_hash_stable is exposed unaggregatable (and CAST to TEXT in SQL,
// since the stdlib emits it as INT64 and the flamegraph reads
// unaggregatable columns as STR_NULL) so it lands in `matchingColumns`
// — that's what lets a PIVOT filter target a specific node by its hash.
// Hidden from the tooltip via `isVisible: false`.
const UNAGG_PROPS = [
  {name: 'root_type', displayName: 'Root Type'},
  {name: 'heap_type', displayName: 'Heap Type'},
  {
    name: 'path_hash_stable',
    displayName: 'Path Hash',
    isVisible: () => false,
  },
];

const SELF_COUNT_AGG_PROP = {
  name: 'self_count',
  displayName: 'Self Count',
  mergeAggregation: 'SUM' as const,
};

// Build a JAVA_HEAP_GRAPH metric for the BFS or dominator class tree,
// projecting `valueColumn` as `value` and the other column for tooltips.
function buildMetric(
  upid: number,
  ts: time,
  name: string,
  unit: string,
  valueColumn: 'self_size' | 'self_count',
  isDominator: boolean,
  showObjectsAction: FlamegraphOptionalAction,
): QueryFlamegraphMetric {
  const tree = isDominator
    ? '_heap_graph_dominator_class_tree'
    : '_heap_graph_class_tree';
  const dependencyModule = isDominator
    ? 'android.memory.heap_graph.dominator_class_tree'
    : 'android.memory.heap_graph.class_tree';
  const otherCol = valueColumn === 'self_size' ? 'self_count' : 'self_size';
  return {
    name,
    unit,
    dependencySql: `include perfetto module ${dependencyModule};`,
    statement: `
      select
        id,
        parent_id as parentId,
        ifnull(name, '[Unknown]') as name,
        root_type,
        heap_type,
        ${valueColumn} as value,
        ${otherCol},
        CAST(path_hash_stable AS TEXT) AS path_hash_stable
      from ${tree}
      where graph_sample_ts = ${ts} and upid = ${upid}
    `,
    unaggregatableProperties: UNAGG_PROPS,
    aggregatableProperties:
      valueColumn === 'self_size' ? [SELF_COUNT_AGG_PROP] : [],
    optionalNodeActions: [showObjectsAction],
  };
}

interface MetricSpec {
  readonly name: string;
  readonly unit: string;
  readonly valueColumn: 'self_size' | 'self_count';
  readonly isDominator: boolean;
}

const METRIC_SPECS: ReadonlyArray<MetricSpec> = [
  {
    name: METRIC_OBJECT_SIZE,
    unit: 'B',
    valueColumn: 'self_size',
    isDominator: false,
  },
  {
    name: 'Object Count',
    unit: '',
    valueColumn: 'self_count',
    isDominator: false,
  },
  {
    name: METRIC_DOMINATED_OBJECT_SIZE,
    unit: 'B',
    valueColumn: 'self_size',
    isDominator: true,
  },
  {
    name: 'Dominated Object Count',
    unit: '',
    valueColumn: 'self_count',
    isDominator: true,
  },
];

function buildHeapGraphMetrics(
  upid: number,
  ts: time,
  onShowObjects: (pathHashes: string, isDominator: boolean) => void,
): ReadonlyArray<QueryFlamegraphMetric> {
  const showObjectsAction = (
    isDominator: boolean,
  ): FlamegraphOptionalAction => ({
    name: 'Show objects from this class',
    execute: async ({properties}) => {
      const pathHashes = properties.get('path_hash_stable');
      if (pathHashes === undefined) return;
      onShowObjects(pathHashes, isDominator);
    },
  });
  return METRIC_SPECS.map((s) =>
    buildMetric(
      upid,
      ts,
      s.name,
      s.unit,
      s.valueColumn,
      s.isDominator,
      showObjectsAction(s.isDominator),
    ),
  );
}

// ---------- Diff metrics (same-trace) -------------------------------------

// Build a JAVA_HEAP_GRAPH diff metric. `value` is `abs(delta)` so width
// reflects movement magnitude; `color_hint` encodes direction.
//
// Pairing uses a name-based path hash recomputed via _graph_scan: the
// stdlib's `path_hash_stable` is hashed from class **ids**, which are
// per-process and not even always shared across dumps of one upid.
// Hashing class names + heap_type instead makes the join key stable
// across processes and across dumps within one trace.
function buildDiffMetric(
  cur: FlamegraphBaselineRef,
  base: FlamegraphBaselineRef,
  name: string,
  unit: string,
  valueColumn: 'self_size' | 'self_count',
  isDominator: boolean,
  showObjectsAction: FlamegraphOptionalAction,
): QueryFlamegraphMetric {
  const tree = isDominator
    ? '_heap_graph_dominator_class_tree'
    : '_heap_graph_class_tree';
  const dependencyModule = isDominator
    ? 'android.memory.heap_graph.dominator_class_tree'
    : 'android.memory.heap_graph.class_tree';
  const dim = valueColumn === 'self_size' ? 'size' : 'count';
  // _graph_scan propagates a hash from each node to its children, where
  // each step folds the child's name + heap_type into the parent hash.
  // The resulting `h` is a path-of-names hash — stable across dumps and
  // processes wherever the same class-name path exists.
  const pathHashScan = (upid: number, ts: number | bigint): string => `
    _graph_scan!(
      (
        SELECT parent_id AS source_node_id, id AS dest_node_id
        FROM ${tree}
        WHERE upid = ${upid} AND graph_sample_ts = ${ts}
          AND parent_id IS NOT NULL
      ),
      (
        SELECT id, HASH(IFNULL(name, ''), IFNULL(heap_type, '')) AS h
        FROM ${tree}
        WHERE upid = ${upid} AND graph_sample_ts = ${ts}
          AND parent_id IS NULL
      ),
      (h),
      (
        SELECT t.id,
               HASH(t.h, IFNULL(c.name, ''), IFNULL(c.heap_type, '')) AS h
        FROM $table t
        JOIN ${tree} c ON c.id = t.id
      )
    )
  `;
  const statement = `
    WITH
    cur_path_hash AS (SELECT * FROM ${pathHashScan(cur.upid, cur.ts)}),
    base_path_hash AS (SELECT * FROM ${pathHashScan(base.upid, base.ts)}),
    cur_nodes AS (
      SELECT t.id, t.parent_id, ifnull(t.name, '[Unknown]') AS name,
             t.root_type, t.heap_type,
             ph.h AS path_h,
             t.self_size AS c_self_size, t.self_count AS c_self_count
      FROM ${tree} t JOIN cur_path_hash ph USING (id)
      WHERE t.upid = ${cur.upid} AND t.graph_sample_ts = ${cur.ts}
    ),
    base_nodes AS (
      SELECT ph.h AS path_h,
             t.self_size AS b_self_size,
             t.self_count AS b_self_count
      FROM ${tree} t JOIN base_path_hash ph USING (id)
      WHERE t.upid = ${base.upid} AND t.graph_sample_ts = ${base.ts}
    ),
    joined AS (
      SELECT
        c.id,
        c.parent_id,
        c.name,
        c.root_type,
        c.heap_type,
        c.path_h,
        c.c_self_size, c.c_self_count,
        ifnull(b.b_self_size, 0) AS b_self_size,
        ifnull(b.b_self_count, 0) AS b_self_count,
        c.c_self_size - ifnull(b.b_self_size, 0) AS delta_size,
        c.c_self_count - ifnull(b.b_self_count, 0) AS delta_count,
        b.path_h IS NULL AS is_new
      FROM cur_nodes c LEFT JOIN base_nodes b USING (path_h)
    ),
    stats AS (SELECT max(abs(delta_${dim})) AS m FROM joined)
    SELECT
      j.id,
      j.parent_id AS parentId,
      j.name,
      j.root_type,
      j.heap_type,
      CAST(j.path_h AS TEXT) AS path_hash_stable,
      abs(j.delta_${dim}) AS value,
      j.c_self_size, j.b_self_size, j.delta_size,
      j.c_self_count, j.b_self_count, j.delta_count,
      -- color_hint format: see getColorSchemeFromHint in flamegraph.ts.
      CASE
        WHEN s.m IS NULL OR s.m = 0 THEN 'palette:u'
        WHEN j.is_new = 1 THEN 'palette:n'
        WHEN j.delta_${dim} = 0 THEN 'palette:u'
        WHEN j.delta_${dim} > 0
          THEN printf('palette:g:%.3f',
            abs(j.delta_${dim}) * 1.0 / s.m)
        ELSE printf('palette:s:%.3f',
            abs(j.delta_${dim}) * 1.0 / s.m)
      END AS color_hint
    FROM joined j CROSS JOIN stats s
  `;
  return {
    name,
    unit,
    dependencySql:
      `include perfetto module ${dependencyModule};\n` +
      `include perfetto module graphs.scan;`,
    statement,
    unaggregatableProperties: UNAGG_PROPS,
    aggregatableProperties: [
      {
        name: 'c_self_size',
        displayName: 'Current Size',
        mergeAggregation: 'SUM' as const,
      },
      {
        name: 'b_self_size',
        displayName: 'Baseline Size',
        mergeAggregation: 'SUM' as const,
      },
      {
        name: 'delta_size',
        displayName: 'Δ Size',
        mergeAggregation: 'SUM' as const,
      },
      {
        name: 'c_self_count',
        displayName: 'Current Count',
        mergeAggregation: 'SUM' as const,
      },
      {
        name: 'b_self_count',
        displayName: 'Baseline Count',
        mergeAggregation: 'SUM' as const,
      },
      {
        name: 'delta_count',
        displayName: 'Δ Count',
        mergeAggregation: 'SUM' as const,
      },
    ],
    optionalNodeActions: [showObjectsAction],
    colorHint: true,
  };
}

function buildHeapGraphDiffMetrics(
  cur: FlamegraphBaselineRef,
  base: FlamegraphBaselineRef,
  onShowObjects: (pathHashes: string, isDominator: boolean) => void,
): ReadonlyArray<QueryFlamegraphMetric> {
  const showObjectsAction = (
    isDominator: boolean,
  ): FlamegraphOptionalAction => ({
    name: 'Show objects from this class',
    execute: async ({properties}) => {
      const pathHashes = properties.get('path_hash_stable');
      if (pathHashes === undefined) return;
      onShowObjects(pathHashes, isDominator);
    },
  });
  // Only nodes present in current are paired here — nodes that exist
  // in baseline but not current (REMOVED) are dropped because they have
  // no place in the current tree's id/parent_id structure. They show up
  // when the user flips primary and baseline.
  return METRIC_SPECS.map((s) =>
    buildDiffMetric(
      cur,
      base,
      `Δ ${s.name}`,
      s.unit,
      s.valueColumn,
      s.isDominator,
      showObjectsAction(s.isDominator),
    ),
  );
}

const FlamegraphView: m.ClosureComponent<FlamegraphViewAttrs> = () => {
  let cachedMetrics: ReadonlyArray<QueryFlamegraphMetric> | undefined;
  let cachedKey: string | undefined;

  return {
    view({attrs}) {
      const baselineKey = attrs.baseline
        ? `${attrs.baseline.upid}:${attrs.baseline.ts}`
        : 'none';
      const key = `${attrs.upid}:${attrs.ts}|${baselineKey}`;
      const metricsChanged = cachedMetrics === undefined || key !== cachedKey;
      if (metricsChanged || cachedMetrics === undefined) {
        cachedMetrics = attrs.baseline
          ? buildHeapGraphDiffMetrics(
              {upid: attrs.upid, ts: attrs.ts},
              attrs.baseline,
              attrs.onShowObjects,
            )
          : buildHeapGraphMetrics(attrs.upid, attrs.ts, attrs.onShowObjects);
        cachedKey = key;
      }
      const metrics: ReadonlyArray<QueryFlamegraphMetric> = cachedMetrics;

      // Either first render OR a dump/baseline change just swapped the
      // metric list. Diff mode renames every metric (`Δ Object Size`
      // etc.), so a stale state.selectedMetricName from before the flip
      // points at a metric that no longer exists. Flamegraph.updateState
      // rebuilds the state, falling back to the first metric when the
      // selection disappeared. Without this the panel either renders
      // nothing or stays on the old metric set.
      let state = attrs.state;
      if (state === undefined || metricsChanged) {
        state = Flamegraph.updateState(state, metrics);
        attrs.onStateChange(state);
      }

      return m(
        'div',
        {class: 'ah-view-content ah-flamegraph-view'},
        m(FlamegraphPanel, {
          trace: attrs.trace,
          metrics,
          state,
          onStateChange: attrs.onStateChange,
        }),
      );
    },
  };
};

export default FlamegraphView;
