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
import type {time} from '../../../base/time';
import type {Engine} from '../../../trace_processor/engine';
import type {QueryFlamegraphMetric} from '../../../components/query_flamegraph';
import {FlamegraphPanel} from '../../../components/flamegraph_panel';
import {
  Flamegraph,
  getDiffColorCss,
  type FlamegraphState,
  type FlamegraphOptionalAction,
} from '../../../widgets/flamegraph';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Callout} from '../../../widgets/callout';
import {Intent} from '../../../widgets/common';
import {Spinner} from '../../../widgets/spinner';
import {prepareCrossTraceDiff} from '../diff/cross_trace_diff';

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
  // size them by |delta|. For same-trace baselines (`baselineEngine`
  // unset), pairing happens in SQL via _graph_scan path hashes. For
  // cross-trace baselines (`baselineEngine` set to a different engine),
  // pairing happens in JS — see diff/cross_trace_diff.ts.
  readonly baseline?: FlamegraphBaselineRef;
  // The engine to fetch the baseline's class tree from. Leave unset for
  // same-trace baselines (the diff SQL queries `attrs.trace.engine`). Set
  // to a different engine for cross-trace pairing.
  readonly baselineEngine?: Engine;
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
        coalesce(name, '<' || coalesce(replace(heap_type, 'HEAP_TYPE_', ''), root_type, 'unnamed') || '>') as name,
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

// pprof-style: each spec produces two flamegraph metrics in diff mode —
// `widthMode: 'current'` keeps width = current dump's value (so the
// flamegraph still reflects today's heap layout) with Δ colors overlaid,
// and `widthMode: 'delta'` switches width to |Δ| (movement magnitude).
// Same colour hint, same pairing CTE, just a different value column.
type WidthMode = 'current' | 'delta';

// How the diff colour score is normalised:
//   'absolute' — Δ / max(|Δ|) across the tree: big movers stand out, units
//                are the metric's own (bytes / count).
//   'relative' — Δ / baseline per node (fractional change), clamped to
//                [-1, 1]; a node absent from the baseline reads as +1.
// The user picks one via the selected metric; colour basis is independent
// of how boxes are sized (WidthMode).
type ColorBasis = 'absolute' | 'relative';

// Build a JAVA_HEAP_GRAPH diff metric. `widthMode='delta'` makes the
// width reflect movement magnitude (|Δ|). `widthMode='current'` keeps
// width = current dump's value so the absolute heap shape is preserved,
// with Δ direction shown in the colour overlay (pprof-style). The
// colour hint is identical in both modes — the difference is only how
// boxes are sized.
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
  widthMode: WidthMode = 'delta',
  colorBasis: ColorBasis = 'absolute',
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
        SELECT id,
               HASH(IFNULL(name, ''), IFNULL(heap_type, ''), IFNULL(root_type, '')) AS h
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
  // MATERIALIZED forces SQLite to actually realise these CTEs (and build
  // automatic indexes on join columns). Without it, the LEFT JOIN below on
  // path_h degenerates to a nested-loop scan — O(N*M), tens of seconds on a
  // single real system_server heap dump. ~20× speedup measured locally.
  const statement = `
    WITH
    cur_path_hash AS MATERIALIZED (SELECT * FROM ${pathHashScan(cur.upid, cur.ts)}),
    base_path_hash AS MATERIALIZED (SELECT * FROM ${pathHashScan(base.upid, base.ts)}),
    cur_nodes AS MATERIALIZED (
      SELECT t.id, t.parent_id,
             coalesce(t.name, '<' || coalesce(replace(t.heap_type, 'HEAP_TYPE_', ''), t.root_type, 'unnamed') || '>') AS name,
             t.root_type, t.heap_type,
             ph.h AS path_h,
             t.self_size AS c_self_size, t.self_count AS c_self_count
      FROM ${tree} t JOIN cur_path_hash ph USING (id)
      WHERE t.upid = ${cur.upid} AND t.graph_sample_ts = ${cur.ts}
    ),
    base_nodes AS MATERIALIZED (
      -- GROUP BY path_h so the LEFT JOIN below can never cross-product
      -- against the base side, even if two base tree nodes happen to share
      -- a path identity. Aggregated values are the natural fold (sum).
      SELECT ph.h AS path_h,
             SUM(t.self_size) AS b_self_size,
             SUM(t.self_count) AS b_self_count
      FROM ${tree} t JOIN base_path_hash ph USING (id)
      WHERE t.upid = ${base.upid} AND t.graph_sample_ts = ${base.ts}
      GROUP BY ph.h
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
      ${
        widthMode === 'current' ? `j.c_self_${dim}` : `abs(j.delta_${dim})`
      } AS value,
      j.c_self_size, j.b_self_size, j.delta_size,
      j.c_self_count, j.b_self_count, j.delta_count,
      -- color_hint = 'diff:<score>' with score in [-1, 1]; see
      -- getColorSchemeFromHint in flamegraph.ts (pprof-style colouring).
      ${
        colorBasis === 'relative'
          ? `CASE
        WHEN j.delta_${dim} = 0 THEN 'diff:0'
        -- No baseline mass (new node) ⇒ unbounded growth ⇒ full red.
        WHEN j.b_self_${dim} = 0 THEN 'diff:1'
        ELSE printf('diff:%.4f',
          max(-1.0, min(1.0, j.delta_${dim} * 1.0 / j.b_self_${dim})))
      END`
          : `CASE
        WHEN s.m IS NULL OR s.m = 0 THEN 'diff:0'
        ELSE printf('diff:%.4f', j.delta_${dim} * 1.0 / s.m)
      END`
      } AS color_hint
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
  // Two metrics per spec — pprof-style:
  //   * absolute (width = current dump's value, colour = Δ direction)
  //     — same flamegraph shape the user saw before engaging diff, with
  //     change information layered in via hue / saturation.
  //   * Δ (width = |Δ|, colour = Δ direction) — emphasises movement.
  //
  // Δ comes first so entering diff mode lands on Δ Object Size, matching
  // the prior behaviour. Both variants share the same CTE / pairing, so
  // SQLite materializes the JOIN once per (dim, isDominator); only the
  // `value` column expression differs.
  //
  // Only nodes present in current are paired here — nodes that exist
  // in baseline but not current (REMOVED) are dropped because they have
  // no place in the current tree's id/parent_id structure. They show up
  // when the user flips primary and baseline.
  // Three metrics per spec, all sharing the same pairing CTE:
  //   * `Δ {name}`            — width = |Δ|, colour = absolute Δ score.
  //   * `Δ {name} (relative)` — width = |Δ|, colour = relative (%) score.
  //   * `{name}`              — width = current value (heap shape kept),
  //                            colour = absolute Δ score.
  // Δ (absolute) comes first so entering diff mode lands on it.
  const metrics: QueryFlamegraphMetric[] = [];
  for (const s of METRIC_SPECS) {
    const action = showObjectsAction(s.isDominator);
    metrics.push(
      buildDiffMetric(
        cur,
        base,
        `Δ ${s.name}`,
        s.unit,
        s.valueColumn,
        s.isDominator,
        action,
        'delta',
        'absolute',
      ),
    );
    metrics.push(
      buildDiffMetric(
        cur,
        base,
        `Δ ${s.name} (relative)`,
        s.unit,
        s.valueColumn,
        s.isDominator,
        action,
        'delta',
        'relative',
      ),
    );
    metrics.push(
      buildDiffMetric(
        cur,
        base,
        s.name,
        s.unit,
        s.valueColumn,
        s.isDominator,
        action,
        'current',
        'absolute',
      ),
    );
  }
  return metrics;
}

// ---------- Cross-trace diff metrics --------------------------------------

// Build a JAVA_HEAP_GRAPH diff metric that reads from a pre-paired temp
// table (see diff/cross_trace_diff.ts) instead of pairing in SQL. The
// downstream value / colour-hint logic is identical to the same-trace
// path — only the source of `joined` differs.
function buildCrossTraceDiffMetric(
  pairedTable: string,
  name: string,
  unit: string,
  valueColumn: 'self_size' | 'self_count',
  showObjectsAction: FlamegraphOptionalAction,
  widthMode: WidthMode = 'delta',
  colorBasis: ColorBasis = 'absolute',
): QueryFlamegraphMetric {
  const dim = valueColumn === 'self_size' ? 'size' : 'count';
  const valExpr =
    widthMode === 'current' ? `j.c_self_${dim}` : `abs(j.delta_${dim})`;
  const colorExpr =
    colorBasis === 'relative'
      ? `CASE
          WHEN j.delta_${dim} = 0 THEN 'diff:0'
          WHEN j.b_self_${dim} = 0 THEN 'diff:1'
          ELSE printf('diff:%.4f',
            max(-1.0, min(1.0, j.delta_${dim} * 1.0 / j.b_self_${dim})))
        END`
      : `CASE
          WHEN s.m IS NULL OR s.m = 0 THEN 'diff:0'
          ELSE printf('diff:%.4f', j.delta_${dim} * 1.0 / s.m)
        END`;
  const statement = `
    WITH
    joined AS (SELECT * FROM ${pairedTable}),
    stats AS (SELECT max(abs(delta_${dim})) AS m FROM joined)
    SELECT
      j.id,
      j.parent_id AS parentId,
      j.name,
      j.root_type,
      j.heap_type,
      j.path_hash_stable,
      ${valExpr} AS value,
      j.c_self_size, j.b_self_size, j.delta_size,
      j.c_self_count, j.b_self_count, j.delta_count,
      ${colorExpr} AS color_hint
    FROM joined j CROSS JOIN stats s
  `;
  return {
    name,
    unit,
    // The temp table is created up-front by prepareCrossTraceDiff. We
    // still need a non-empty dependencySql for the QueryFlamegraph
    // machinery, so do a harmless no-op.
    dependencySql: 'SELECT 1;',
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

// Cross-trace counterpart of buildHeapGraphDiffMetrics. Same set of 12
// metrics (4 specs × {Δ, Δ-relative, current-width}), each pointed at the
// appropriate pre-paired temp table.
function buildCrossTraceHeapGraphDiffMetrics(
  classTreeTable: string,
  dominatorTreeTable: string,
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
  const metrics: QueryFlamegraphMetric[] = [];
  for (const s of METRIC_SPECS) {
    const table = s.isDominator ? dominatorTreeTable : classTreeTable;
    const action = showObjectsAction(s.isDominator);
    metrics.push(
      buildCrossTraceDiffMetric(
        table,
        `Δ ${s.name}`,
        s.unit,
        s.valueColumn,
        action,
        'delta',
        'absolute',
      ),
    );
    metrics.push(
      buildCrossTraceDiffMetric(
        table,
        `Δ ${s.name} (relative)`,
        s.unit,
        s.valueColumn,
        action,
        'delta',
        'relative',
      ),
    );
    metrics.push(
      buildCrossTraceDiffMetric(
        table,
        s.name,
        s.unit,
        s.valueColumn,
        action,
        'current',
        'absolute',
      ),
    );
  }
  return metrics;
}

// pprof-style diff legend: a green→grey→red gradient painted from the
// same getDiffColorCss() the flamegraph node fills use, so the legend and
// the boxes always agree. Shown only in diff mode.
function renderDiffLegend(relative: boolean): m.Child {
  const stops = [-1, -0.66, -0.33, 0, 0.33, 0.66, 1]
    .map((s) => getDiffColorCss(s))
    .join(', ');
  return m(
    '.pf-hde-diff-legend',
    m('span.pf-hde-diff-legend__title', 'Change vs baseline'),
    m('span.pf-hde-diff-legend__end', 'smaller'),
    m('.pf-hde-diff-legend__bar', {
      style: {background: `linear-gradient(to right, ${stops})`},
    }),
    m('span.pf-hde-diff-legend__end', 'larger'),
    // Standard Perfetto Button widget — matches the dump-selector buttons
    // on this page (compact outlined). Non-interactive — purely a label
    // indicating the current colour basis.
    m(Button, {
      label: relative ? 'relative (%)' : 'absolute',
      variant: ButtonVariant.Outlined,
      compact: true,
      disabled: true,
    }),
  );
}

// Per-component counter for cross-trace temp table names. Engine-global
// uniqueness, not cryptographic; just needs to not collide across
// re-prepares within one engine.
let xTraceTableSeq = 0;

const FlamegraphView: m.ClosureComponent<FlamegraphViewAttrs> = () => {
  let cachedMetrics: ReadonlyArray<QueryFlamegraphMetric> | undefined;
  let cachedKey: string | undefined;

  // Cross-trace prep state. `key` matches the metric key when prep has
  // completed for the current (cur, base, engines) tuple, so a stale
  // baseline change leaves the old prep visibly out-of-date and we'll
  // re-prep.
  let prep:
    | {
        readonly key: string;
        status: 'pending' | 'ready' | 'error';
        classTable?: string;
        dominatorTable?: string;
        error?: Error;
      }
    | undefined;

  return {
    view({attrs}) {
      const isCrossTrace =
        attrs.baseline !== undefined && attrs.baselineEngine !== undefined;
      const baselineKey = attrs.baseline
        ? `${attrs.baseline.upid}:${attrs.baseline.ts}`
        : 'none';
      // The cross-trace key includes the baseline engine in `isCrossTrace`
      // so swapping baseline traces invalidates the prep.
      const key = `${attrs.upid}:${attrs.ts}|${baselineKey}|${
        isCrossTrace ? 'x' : 's'
      }`;

      // Kick off cross-trace prep if needed.
      if (isCrossTrace && (prep === undefined || prep.key !== key)) {
        const seq = xTraceTableSeq++;
        const classTable = `_x_diff_class_${seq}`;
        const dominatorTable = `_x_diff_dom_${seq}`;
        const myPrep: NonNullable<typeof prep> = {key, status: 'pending'};
        prep = myPrep;
        const curRef = {upid: attrs.upid, ts: BigInt(attrs.ts)};
        const baseRef = {
          upid: attrs.baseline!.upid,
          ts: BigInt(attrs.baseline!.ts),
        };
        Promise.all([
          prepareCrossTraceDiff(
            classTable,
            attrs.trace.engine,
            curRef,
            attrs.baselineEngine!,
            baseRef,
            '_heap_graph_class_tree',
          ),
          prepareCrossTraceDiff(
            dominatorTable,
            attrs.trace.engine,
            curRef,
            attrs.baselineEngine!,
            baseRef,
            '_heap_graph_dominator_class_tree',
          ),
        ])
          .then(() => {
            // Ignore the result if a newer prep has taken over.
            if (prep !== myPrep) return;
            myPrep.status = 'ready';
            myPrep.classTable = classTable;
            myPrep.dominatorTable = dominatorTable;
            // Force the metric cache to rebuild — `key` already matches.
            cachedMetrics = undefined;
            cachedKey = undefined;
            m.redraw();
          })
          .catch((err: Error) => {
            if (prep !== myPrep) return;
            myPrep.status = 'error';
            myPrep.error = err;
            m.redraw();
          });
      }
      if (!isCrossTrace) prep = undefined;

      if (isCrossTrace && prep !== undefined && prep.status === 'pending') {
        return m(
          'div',
          {class: 'pf-hde-view-content pf-hde-flamegraph-view'},
          m(
            Callout,
            {icon: 'memory', intent: Intent.None},
            m(Spinner, {easing: true}),
            ' Preparing cross-trace diff: pairing class trees…',
          ),
        );
      }
      if (isCrossTrace && prep !== undefined && prep.status === 'error') {
        return m(
          'div',
          {class: 'pf-hde-view-content pf-hde-flamegraph-view'},
          m(
            Callout,
            {icon: 'error', intent: Intent.Danger},
            `Cross-trace diff failed: ${prep.error?.message ?? 'unknown error'}`,
          ),
        );
      }

      const metricsChanged = cachedMetrics === undefined || key !== cachedKey;
      if (metricsChanged || cachedMetrics === undefined) {
        if (isCrossTrace && prep?.status === 'ready') {
          cachedMetrics = buildCrossTraceHeapGraphDiffMetrics(
            prep.classTable!,
            prep.dominatorTable!,
            attrs.onShowObjects,
          );
        } else if (attrs.baseline) {
          cachedMetrics = buildHeapGraphDiffMetrics(
            {upid: attrs.upid, ts: attrs.ts},
            attrs.baseline,
            attrs.onShowObjects,
          );
        } else {
          cachedMetrics = buildHeapGraphMetrics(
            attrs.upid,
            attrs.ts,
            attrs.onShowObjects,
          );
        }
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

      const legend = attrs.baseline
        ? renderDiffLegend(state.selectedMetricName.includes('(relative)'))
        : null;

      return m(
        'div',
        {class: 'pf-hde-view-content pf-hde-flamegraph-view'},
        legend,
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
