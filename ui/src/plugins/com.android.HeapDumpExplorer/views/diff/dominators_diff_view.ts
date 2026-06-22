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

// Dominators diff: GROUP BY root class name. Root object ids aren't
// stable across snapshots, so class is the natural join key.

import m from 'mithril';
import {Spinner} from '../../../../widgets/spinner';
import {EmptyState} from '../../../../widgets/empty_state';
import type {Engine} from '../../../../trace_processor/engine';
import {NUM, STR_NULL} from '../../../../trace_processor/query_result';
import type {Row} from '../../../../trace_processor/query_result';
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
import {SQL_PREAMBLE} from '../../components';
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

function buildQuery(filterSql: string): string {
  return `
    SELECT
      ifnull(cls.deobfuscated_name, cls.name) AS root_class,
      COUNT(*) AS cnt,
      SUM(d.dominated_size_bytes) AS dominated_size_bytes,
      SUM(d.dominated_native_size_bytes) AS dominated_native_size_bytes,
      SUM(d.dominated_obj_count) AS dominated_obj_count
    FROM heap_graph_dominator_tree d
    JOIN heap_graph_object o ON o.id = d.id
    JOIN heap_graph_class cls ON cls.id = o.type_id
    WHERE d.idom_id IS NULL AND ${filterSql}
    GROUP BY root_class
  `;
}

const ITER_SPEC = {
  root_class: STR_NULL,
  cnt: NUM,
  dominated_size_bytes: NUM,
  dominated_native_size_bytes: NUM,
  dominated_obj_count: NUM,
};

interface DominatorsDiffViewAttrs {
  readonly currentEngine: Engine;
  readonly baselineEngine: Engine;
  readonly navigate: NavFn;
}

const NUMERIC_FIELDS = [
  'cnt',
  'dominated_size_bytes',
  'dominated_native_size_bytes',
  'dominated_obj_count',
];

// Single-engine fast path: one SQL JOIN, no JS merge.
function buildDiffJoinQuery(
  cur: {upid: number; ts: number | bigint},
  base: {upid: number; ts: number | bigint},
): string {
  const side = (name: string, upid: number, ts: number | bigint): string => `
    ${name} AS (
      SELECT
        ifnull(cls.deobfuscated_name, cls.name) AS k,
        COUNT(*) AS cnt,
        SUM(d.dominated_size_bytes) AS dominated_size_bytes,
        SUM(d.dominated_native_size_bytes) AS dominated_native_size_bytes,
        SUM(d.dominated_obj_count) AS dominated_obj_count
      FROM heap_graph_dominator_tree d
      JOIN heap_graph_object o ON o.id = d.id
      JOIN heap_graph_class cls ON cls.id = o.type_id
      WHERE d.idom_id IS NULL
        AND o.upid = ${upid} AND o.graph_sample_ts = ${ts}
        AND ifnull(cls.deobfuscated_name, cls.name) IS NOT NULL
      GROUP BY k
    )`;
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
        c.k AS root_class,
        b.cnt AS ${baselineCol('cnt')},
        c.cnt AS ${currentCol('cnt')},
        c.cnt - ifnull(b.cnt, 0) AS ${deltaCol('cnt')},
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
        b.cnt, NULL, -b.cnt,
        b.dominated_size_bytes, NULL, -b.dominated_size_bytes,
        b.dominated_native_size_bytes, NULL, -b.dominated_native_size_bytes,
        b.dominated_obj_count, NULL, -b.dominated_obj_count
      FROM base b
      WHERE NOT EXISTS (SELECT 1 FROM cur c WHERE c.k = b.k)
    )
    ORDER BY abs(${deltaCol('dominated_size_bytes')}) DESC
  `;
}

const DIFF_ROW_ITER = {
  [KEY_COL]: STR_NULL,
  [STATUS_COL]: STR_NULL,
  root_class: STR_NULL,
  [baselineCol('cnt')]: LONG_NULL,
  [currentCol('cnt')]: LONG_NULL,
  [deltaCol('cnt')]: LONG,
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

async function fetchDiffViaSql(
  engine: Engine,
  cur: {upid: number; ts: number | bigint},
  base: {upid: number; ts: number | bigint},
): Promise<DiffRow[]> {
  const key = `dominators-diff:${dumpKey(cur.upid, cur.ts)}:${dumpKey(base.upid, base.ts)}`;
  const rows = await cachedFetch(engine, key, async () => {
    await engine.query(SQL_PREAMBLE);
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

// Cached per (engine, dump). The primary side reuses across baseline
// swaps; see diff_cache.ts.
async function runQuery(
  engine: Engine,
  upid: number,
  ts: number | bigint,
  filterSql: string,
): Promise<ReadonlyArray<Row>> {
  return cachedFetch(engine, `dominators:${dumpKey(upid, ts)}`, async () => {
    await engine.query(SQL_PREAMBLE);
    const res = await engine.query(buildQuery(filterSql));
    const out: Row[] = [];
    const it = res.iter(ITER_SPEC);
    for (; it.valid(); it.next()) {
      if (it.root_class === null) continue;
      out.push({
        root_class: it.root_class,
        cnt: it.cnt,
        dominated_size_bytes: it.dominated_size_bytes,
        dominated_native_size_bytes: it.dominated_native_size_bytes,
        dominated_obj_count: it.dominated_obj_count,
      });
    }
    return dedupeByKey(out, (r) => String(r.root_class ?? ''), NUMERIC_FIELDS);
  });
}

function DominatorsDiffView(): m.Component<DominatorsDiffViewAttrs> {
  let rows: DiffRow[] | null = null;
  let loading = false;
  let error: string | null = null;
  let dataSource: InMemoryDataSource | null = null;
  let lastB: Engine | null = null;
  let lastC: Engine | null = null;

  async function load(currentEngine: Engine, baselineEngine: Engine) {
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
        merged = await fetchDiffViaSql(
          currentEngine,
          {upid: primarySnap.upid, ts: primarySnap.ts},
          {upid: baselineSnap.dump.upid, ts: baselineSnap.dump.ts},
        );
      } else {
        const baselineFilter = baselineDumpFilterSql('o');
        const currentFilter = dumpFilterSql(primarySnap, 'o');
        const [b, c] = await Promise.all([
          runQuery(
            baselineEngine,
            baselineSnap.dump.upid,
            baselineSnap.dump.ts,
            baselineFilter,
          ),
          runQuery(
            currentEngine,
            primarySnap.upid,
            primarySnap.ts,
            currentFilter,
          ),
        ]);
        if (isStale()) return;
        merged = mergeRows({
          baseline: b,
          current: c,
          keyOf: (r) => String(r.root_class ?? ''),
          numericFields: NUMERIC_FIELDS,
          primaryDeltaField: 'dominated_size_bytes',
        });
        merged.sort(compareByAbsDeltaDesc('dominated_size_bytes'));
      }
      if (isStale()) return;
      rows = merged;
      dataSource = new InMemoryDataSource(merged);
      publishDiffRows('dominators', merged);
    } catch (err) {
      if (isStale()) return;
      error = err instanceof Error ? err.message : String(err);
      console.error('Dominators diff load failed:', err);
    } finally {
      loading = false;
      m.redraw();
    }
  }

  function ensure(currentEngine: Engine, baselineEngine: Engine) {
    if (currentEngine !== lastC || baselineEngine !== lastB) {
      lastC = currentEngine;
      lastB = baselineEngine;
      rows = null;
      dataSource = null;
      load(currentEngine, baselineEngine).catch(console.error);
    }
  }

  return {
    oninit(vnode) {
      ensure(vnode.attrs.currentEngine, vnode.attrs.baselineEngine);
    },
    onupdate(vnode) {
      ensure(vnode.attrs.currentEngine, vnode.attrs.baselineEngine);
    },
    view(vnode) {
      const {navigate} = vnode.attrs;
      if (loading && !rows) {
        return m('div', {class: 'pf-hde-loading'}, m(Spinner, {easing: true}));
      }
      if (error) {
        return m(EmptyState, {
          icon: 'error',
          title: `Failed to compute Dominators diff: ${error}`,
          fillHeight: true,
        });
      }
      if (!rows || !dataSource) {
        return m(EmptyState, {
          icon: 'account_tree',
          title: 'No dominator-tree roots to diff',
          fillHeight: true,
        });
      }

      const size = {
        field: 'dominated_size_bytes',
        title: 'Retained',
        kind: 'size' as const,
      };
      const count = {
        field: 'cnt',
        title: 'Roots',
        kind: 'count' as const,
      };
      const extraFields = [
        {
          field: 'dominated_native_size_bytes',
          title: 'Retained Native',
          kind: 'size' as const,
        },
        {
          field: 'dominated_obj_count',
          title: 'Retained Count',
          kind: 'count' as const,
        },
      ];
      const schema = buildSizeCountSchema({
        keyTitle: 'Root Class',
        keyRenderer: (value) =>
          m(
            'button',
            {
              class: 'pf-hde-link',
              onclick: () => navigate('objects', {cls: String(value ?? '')}),
            },
            String(value ?? ''),
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
          'Dominators diff ',
          m(
            'span',
            {class: 'pf-hde-muted'},
            `(${rows.length.toLocaleString()} root classes)`,
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

export default DominatorsDiffView;
