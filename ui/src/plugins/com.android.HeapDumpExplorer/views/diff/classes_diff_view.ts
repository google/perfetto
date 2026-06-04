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

// Classes diff: runs the same `android_heap_graph_class_aggregation`
// query the single-engine view uses, with no LIMIT, then merges by
// class name.

import m from 'mithril';
import {Spinner} from '../../../../widgets/spinner';
import {EmptyState} from '../../../../widgets/empty_state';
import type {Engine} from '../../../../trace_processor/engine';
import type {Row} from '../../../../trace_processor/query_result';
import {
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../../../trace_processor/query_result';
import {DataGrid} from '../../../../components/widgets/datagrid/datagrid';
import {InMemoryDataSource} from '../../../../components/widgets/datagrid/in_memory_data_source';
import type {NavFn} from '../../components';
import type {DiffRow} from '../../diff/diff_rows';
import {
  compareByAbsDeltaDesc,
  dedupeByKey,
  mergeRows,
} from '../../diff/diff_rows';
import {publishDiffRows} from '../../diff/diff_debug';
import {
  buildSizeCountInitialColumns,
  buildSizeCountSchema,
} from '../../diff/diff_schemas';
import {
  baselineDumpFilterSql,
  getActiveBaseline,
  isSelfTraceDiff,
} from '../../baseline/state';
import {dumpFilterSql, getActiveDump} from '../../queries';
import {cachedFetch, dumpKey} from '../../diff/diff_cache';
import {
  KEY_COL,
  STATUS_COL,
  baselineCol,
  currentCol,
  deltaCol,
} from '../../diff/diff_rows';
import {LONG, LONG_NULL} from '../../../../trace_processor/query_result';

const PREAMBLE =
  'INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_class_aggregation';

function buildQuery(filterSql: string): string {
  return `
    SELECT
      type_name,
      reachable_obj_count,
      reachable_size_bytes,
      reachable_native_size_bytes,
      dominated_size_bytes,
      dominated_native_size_bytes,
      dominated_obj_count
    FROM android_heap_graph_class_aggregation a
    WHERE a.reachable_obj_count > 0 AND ${filterSql}
  `;
}

const ITER_SPEC = {
  // NULL when both name and deobfuscated_name are missing; rows skipped
  // since there's no usable join key without a class name.
  type_name: STR_NULL,
  reachable_obj_count: NUM,
  reachable_size_bytes: NUM,
  reachable_native_size_bytes: NUM_NULL,
  dominated_size_bytes: NUM_NULL,
  dominated_native_size_bytes: NUM_NULL,
  dominated_obj_count: NUM_NULL,
};

const NUMERIC_FIELDS = [
  'reachable_obj_count',
  'reachable_size_bytes',
  'reachable_native_size_bytes',
  'dominated_size_bytes',
  'dominated_native_size_bytes',
  'dominated_obj_count',
];

interface ClassesDiffViewAttrs {
  readonly currentEngine: Engine;
  readonly baselineEngine: Engine;
  readonly navigate: NavFn;
}

// SQL-side diff: when both dumps live in the same trace processor we
// avoid the two-query + JS merge round-trip entirely. The query LEFT
// JOINs cur on base by class name, then UNION ALL appends the anti-join
// (REMOVED rows that only exist in base). Status + _b_/_c_/_d_ columns
// are computed in SQLite, so the JS side just passes them to the grid.
function buildDiffJoinQuery(
  cur: {upid: number; ts: number | bigint},
  base: {upid: number; ts: number | bigint},
): string {
  const reachable = `a.reachable_obj_count > 0 AND a.type_name IS NOT NULL`;
  // Per-side aggregates. type_name may repeat across class-loaders, so
  // we sum-group by name to dedupe — equivalent to the JS `dedupeByKey`
  // step in the cross-trace path.
  const side = (name: string, upid: number, ts: number | bigint): string => `
    ${name} AS (
      SELECT
        a.type_name AS k,
        sum(a.reachable_obj_count) AS reachable_obj_count,
        sum(a.reachable_size_bytes) AS reachable_size_bytes,
        sum(a.reachable_native_size_bytes) AS reachable_native_size_bytes,
        sum(a.dominated_size_bytes) AS dominated_size_bytes,
        sum(a.dominated_native_size_bytes) AS dominated_native_size_bytes,
        sum(a.dominated_obj_count) AS dominated_obj_count
      FROM android_heap_graph_class_aggregation a
      WHERE a.upid = ${upid} AND a.graph_sample_ts = ${ts} AND ${reachable}
      GROUP BY a.type_name
    )`;
  // Compute every diff row in SQL. UNION ALL combines the
  // cur-LEFT-JOIN-base set (GREW / SHRANK / UNCHANGED / NEW) with the
  // base-anti-join-cur set (REMOVED). SQLite requires ORDER BY on a
  // compound SELECT to reference column positions or first-branch
  // names; wrapping in a subquery sidesteps that and makes the
  // ordering obvious.
  return `
    SELECT * FROM (
      WITH
      ${side('cur', cur.upid, cur.ts)},
      ${side('base', base.upid, base.ts)}
      SELECT
        c.k AS ${KEY_COL},
        CASE
          WHEN b.k IS NULL THEN 'NEW'
          WHEN c.dominated_size_bytes = b.dominated_size_bytes THEN 'UNCHANGED'
          WHEN c.dominated_size_bytes > b.dominated_size_bytes THEN 'GREW'
          ELSE 'SHRANK'
        END AS ${STATUS_COL},
        c.k AS type_name,
        b.reachable_obj_count AS ${baselineCol('reachable_obj_count')},
        c.reachable_obj_count AS ${currentCol('reachable_obj_count')},
        c.reachable_obj_count - ifnull(b.reachable_obj_count, 0)
          AS ${deltaCol('reachable_obj_count')},
        b.reachable_size_bytes AS ${baselineCol('reachable_size_bytes')},
        c.reachable_size_bytes AS ${currentCol('reachable_size_bytes')},
        c.reachable_size_bytes - ifnull(b.reachable_size_bytes, 0)
          AS ${deltaCol('reachable_size_bytes')},
        b.reachable_native_size_bytes AS ${baselineCol('reachable_native_size_bytes')},
        c.reachable_native_size_bytes AS ${currentCol('reachable_native_size_bytes')},
        c.reachable_native_size_bytes - ifnull(b.reachable_native_size_bytes, 0)
          AS ${deltaCol('reachable_native_size_bytes')},
        b.dominated_size_bytes AS ${baselineCol('dominated_size_bytes')},
        c.dominated_size_bytes AS ${currentCol('dominated_size_bytes')},
        c.dominated_size_bytes - ifnull(b.dominated_size_bytes, 0)
          AS ${deltaCol('dominated_size_bytes')},
        b.dominated_native_size_bytes AS ${baselineCol('dominated_native_size_bytes')},
        c.dominated_native_size_bytes AS ${currentCol('dominated_native_size_bytes')},
        c.dominated_native_size_bytes - ifnull(b.dominated_native_size_bytes, 0)
          AS ${deltaCol('dominated_native_size_bytes')},
        b.dominated_obj_count AS ${baselineCol('dominated_obj_count')},
        c.dominated_obj_count AS ${currentCol('dominated_obj_count')},
        c.dominated_obj_count - ifnull(b.dominated_obj_count, 0)
          AS ${deltaCol('dominated_obj_count')}
      FROM cur c LEFT JOIN base b USING (k)
      UNION ALL
      SELECT
        b.k,
        'REMOVED',
        b.k,
        b.reachable_obj_count,
        NULL,
        -b.reachable_obj_count,
        b.reachable_size_bytes,
        NULL,
        -b.reachable_size_bytes,
        b.reachable_native_size_bytes,
        NULL,
        -b.reachable_native_size_bytes,
        b.dominated_size_bytes,
        NULL,
        -b.dominated_size_bytes,
        b.dominated_native_size_bytes,
        NULL,
        -b.dominated_native_size_bytes,
        b.dominated_obj_count,
        NULL,
        -b.dominated_obj_count
      FROM base b
      WHERE NOT EXISTS (SELECT 1 FROM cur c WHERE c.k = b.k)
    )
    ORDER BY abs(${deltaCol('dominated_size_bytes')}) DESC
  `;
}

const DIFF_ROW_ITER = {
  [KEY_COL]: STR_NULL,
  [STATUS_COL]: STR_NULL,
  type_name: STR_NULL,
  // Per-row "is the value nullable" varies — NEW has null baseline,
  // REMOVED has null current. LONG_NULL across the board is safe.
  [baselineCol('reachable_obj_count')]: LONG_NULL,
  [currentCol('reachable_obj_count')]: LONG_NULL,
  [deltaCol('reachable_obj_count')]: LONG,
  [baselineCol('reachable_size_bytes')]: LONG_NULL,
  [currentCol('reachable_size_bytes')]: LONG_NULL,
  [deltaCol('reachable_size_bytes')]: LONG,
  [baselineCol('reachable_native_size_bytes')]: LONG_NULL,
  [currentCol('reachable_native_size_bytes')]: LONG_NULL,
  [deltaCol('reachable_native_size_bytes')]: LONG,
  [baselineCol('dominated_size_bytes')]: LONG_NULL,
  [currentCol('dominated_size_bytes')]: LONG_NULL,
  [deltaCol('dominated_size_bytes')]: LONG,
  [baselineCol('dominated_native_size_bytes')]: LONG_NULL,
  [currentCol('dominated_native_size_bytes')]: LONG_NULL,
  [deltaCol('dominated_native_size_bytes')]: LONG,
  [baselineCol('dominated_obj_count')]: LONG_NULL,
  [currentCol('dominated_obj_count')]: LONG_NULL,
  [deltaCol('dominated_obj_count')]: LONG,
};

// Same-trace fast path. Runs ONE query, gets DiffRows back already
// shaped + sorted. Cached by (engine, cur dump, base dump).
async function fetchDiffViaSql(
  engine: Engine,
  cur: {upid: number; ts: number | bigint},
  base: {upid: number; ts: number | bigint},
): Promise<DiffRow[]> {
  const key = `classes-diff:${dumpKey(cur.upid, cur.ts)}:${dumpKey(base.upid, base.ts)}`;
  const rows = await cachedFetch(engine, key, async () => {
    await engine.query(PREAMBLE);
    const res = await engine.query(buildDiffJoinQuery(cur, base));
    const out: Row[] = [];
    for (const it = res.iter(DIFF_ROW_ITER); it.valid(); it.next()) {
      const row: Record<string, unknown> = {};
      for (const k of Object.keys(DIFF_ROW_ITER)) {
        row[k] = it[k as keyof typeof DIFF_ROW_ITER];
      }
      out.push(row as Row);
    }
    return out;
  });
  return rows as unknown as DiffRow[];
}

// Cached per (engine, dump). Same primary dump across baseline swaps
// reuses these rows on the second click of Classes — first load on a
// 60 MB hprof is ~7 s; cached re-visit is sub-millisecond.
async function fetchAll(
  engine: Engine,
  upid: number,
  ts: number | bigint,
  filterSql: string,
): Promise<ReadonlyArray<Row>> {
  return cachedFetch(engine, `classes:${dumpKey(upid, ts)}`, async () => {
    await engine.query(PREAMBLE);
    const res = await engine.query(buildQuery(filterSql));
    const out: Row[] = [];
    for (const it = res.iter(ITER_SPEC); it.valid(); it.next()) {
      if (it.type_name === null) continue;
      out.push({
        type_name: it.type_name,
        reachable_obj_count: it.reachable_obj_count,
        reachable_size_bytes: it.reachable_size_bytes,
        reachable_native_size_bytes: it.reachable_native_size_bytes,
        dominated_size_bytes: it.dominated_size_bytes,
        dominated_native_size_bytes: it.dominated_native_size_bytes,
        dominated_obj_count: it.dominated_obj_count,
      });
    }
    return dedupeByKey(out, (r) => String(r.type_name ?? ''), NUMERIC_FIELDS);
  });
}

function ClassesDiffView(): m.Component<ClassesDiffViewAttrs> {
  let rows: DiffRow[] | null = null;
  let loading = false;
  let error: string | null = null;
  let dataSource: InMemoryDataSource | null = null;

  let lastBaselineEngine: Engine | null = null;
  let lastCurrentEngine: Engine | null = null;

  async function load(currentEngine: Engine, baselineEngine: Engine) {
    // Snapshot active refs; any change during the awaits means the
    // result is stale and we drop it. The same check in catch silently
    // swallows rejections from a disposed engine.
    const primarySnap = getActiveDump();
    const baselineSnap = getActiveBaseline();
    if (!primarySnap || !baselineSnap) return;
    const isStale = () =>
      getActiveDump() !== primarySnap || getActiveBaseline() !== baselineSnap;
    loading = true;
    error = null;
    try {
      let merged: DiffRow[];
      if (isSelfTraceDiff()) {
        // Single-engine fast path: one SQL JOIN, no JS merge. SQLite
        // produces DiffRow-shaped output directly and orders by
        // |Δ dominated_size_bytes| desc.
        merged = await fetchDiffViaSql(
          currentEngine,
          {upid: primarySnap.upid, ts: primarySnap.ts},
          {upid: baselineSnap.dump.upid, ts: baselineSnap.dump.ts},
        );
      } else {
        // Cross-trace path: two engines, JS outer-join in mergeRows.
        // Pass primarySnap explicitly. The bare
        // `dumpFilterSql(undefined)` form fell back to
        // dumpFilterOverride which is null in diff mode — yielding
        // '1=1' and pulling in every dump in the primary engine.
        const baselineFilter = baselineDumpFilterSql('a');
        const currentFilter = dumpFilterSql(primarySnap, 'a');
        const [baselineRows, currentRows] = await Promise.all([
          fetchAll(
            baselineEngine,
            baselineSnap.dump.upid,
            baselineSnap.dump.ts,
            baselineFilter,
          ),
          fetchAll(
            currentEngine,
            primarySnap.upid,
            primarySnap.ts,
            currentFilter,
          ),
        ]);
        if (isStale()) return;
        merged = mergeRows({
          baseline: baselineRows,
          current: currentRows,
          keyOf: (r) => String(r.type_name ?? ''),
          numericFields: NUMERIC_FIELDS,
          primaryDeltaField: 'dominated_size_bytes',
        });
        merged.sort(compareByAbsDeltaDesc('dominated_size_bytes'));
      }
      if (isStale()) return;
      rows = merged;
      dataSource = new InMemoryDataSource(merged);
      publishDiffRows('classes', merged);
    } catch (err) {
      if (isStale()) return;
      error = err instanceof Error ? err.message : String(err);
      console.error('Classes diff load failed:', err);
    } finally {
      loading = false;
      m.redraw();
    }
  }

  function ensureLoaded(currentEngine: Engine, baselineEngine: Engine) {
    if (
      currentEngine !== lastCurrentEngine ||
      baselineEngine !== lastBaselineEngine
    ) {
      lastCurrentEngine = currentEngine;
      lastBaselineEngine = baselineEngine;
      rows = null;
      dataSource = null;
      load(currentEngine, baselineEngine).catch(console.error);
    }
  }

  return {
    oninit(vnode) {
      ensureLoaded(vnode.attrs.currentEngine, vnode.attrs.baselineEngine);
    },
    onupdate(vnode) {
      ensureLoaded(vnode.attrs.currentEngine, vnode.attrs.baselineEngine);
    },
    view(vnode) {
      const {navigate} = vnode.attrs;
      if (loading && !rows) {
        return m('div', {class: 'pf-hde-loading'}, m(Spinner, {easing: true}));
      }
      if (error) {
        return m(EmptyState, {
          icon: 'error',
          title: `Failed to compute Classes diff: ${error}`,
          fillHeight: true,
        });
      }
      if (!rows || !dataSource) {
        return m(EmptyState, {
          icon: 'memory',
          title: 'No Java heap data to diff',
          fillHeight: true,
        });
      }

      // Same column titles as the non-diff Classes view (`Retained`,
      // `Count`, `Reachable`, etc.) so users moving between modes see
      // the same vocabulary, just with Δ/Baseline/Current prefixes.
      const size = {
        field: 'dominated_size_bytes',
        title: 'Retained',
        kind: 'size' as const,
      };
      const count = {
        field: 'reachable_obj_count',
        title: 'Count',
        kind: 'count' as const,
      };
      const extraFields = [
        {
          field: 'reachable_size_bytes',
          title: 'Reachable',
          kind: 'size' as const,
        },
        {
          field: 'dominated_obj_count',
          title: 'Retained Count',
          kind: 'count' as const,
        },
      ];
      const schema = buildSizeCountSchema({
        keyTitle: 'Class',
        keyRenderer: (value) =>
          m(
            'button',
            {
              class: 'pf-hde-link',
              onclick: () => navigate('objects', {cls: String(value)}),
            },
            String(value),
          ),
        size,
        count,
        extraFields,
      });

      const initialColumns = buildSizeCountInitialColumns({
        size,
        count,
        extraFields,
      });

      return m('div', {class: 'pf-hde-view-content'}, [
        m('h2', {class: 'pf-hde-view-heading'}, [
          'Classes diff ',
          m(
            'span',
            {class: 'pf-hde-muted'},
            `(${rows.length.toLocaleString()} classes)`,
          ),
        ]),
        m(DataGrid, {
          schema,
          rootSchema: 'query',
          data: dataSource,
          fillHeight: true,
          initialColumns,
          showExportButton: true,
        }),
      ]);
    },
  };
}

export default ClassesDiffView;
