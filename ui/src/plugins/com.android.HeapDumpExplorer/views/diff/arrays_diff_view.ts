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

// Arrays diff: GROUP BY array_data_hash (content hash) per engine,
// JS outer-join.

import m from 'mithril';
import {Spinner} from '../../../../widgets/spinner';
import {EmptyState} from '../../../../widgets/empty_state';
import type {Engine} from '../../../../trace_processor/engine';
import {LONG_NULL, NUM} from '../../../../trace_processor/query_result';
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
import {baselineDumpFilterSql, getActiveBaseline} from '../../baseline/state';
import {dumpFilterSql, getActiveDump} from '../../queries';

function buildQuery(filterSql: string): string {
  return `
    SELECT
      od.array_data_hash AS h,
      COUNT(*) AS cnt,
      SUM(o.self_size) AS retained
    FROM heap_graph_object o
    JOIN heap_graph_object_data od ON o.object_data_id = od.id
    WHERE o.reachable != 0
      AND ${filterSql}
      AND od.array_data_hash IS NOT NULL
    GROUP BY od.array_data_hash
  `;
}

const ITER_SPEC = {h: LONG_NULL, cnt: NUM, retained: NUM};

interface ArraysDiffViewAttrs {
  readonly currentEngine: Engine;
  readonly baselineEngine: Engine;
  readonly navigate: NavFn;
}

const NUMERIC_FIELDS = ['cnt', 'retained'];

async function runQuery(engine: Engine, filterSql: string): Promise<Row[]> {
  const res = await engine.query(buildQuery(filterSql));
  const out: Row[] = [];
  for (const it = res.iter(ITER_SPEC); it.valid(); it.next()) {
    if (it.h === null) continue;
    out.push({
      h: it.h.toString(),
      cnt: it.cnt,
      retained: it.retained,
    });
  }
  return dedupeByKey(out, (r) => String(r.h ?? ''), NUMERIC_FIELDS);
}

function ArraysDiffView(): m.Component<ArraysDiffViewAttrs> {
  let rows: DiffRow[] | null = null;
  let loading = false;
  let error: string | null = null;
  let dataSource: InMemoryDataSource | null = null;
  let lastB: Engine | null = null;
  let lastC: Engine | null = null;

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
      const baselineFilter = baselineDumpFilterSql('o');
      const currentFilter = dumpFilterSql(primarySnap, 'o');
      const [b, c] = await Promise.all([
        runQuery(baselineEngine, baselineFilter),
        runQuery(currentEngine, currentFilter),
      ]);
      if (isStale()) return;
      const merged = mergeRows({
        baseline: b,
        current: c,
        keyOf: (r) => String(r.h ?? ''),
        numericFields: NUMERIC_FIELDS,
        primaryDeltaField: 'retained',
      });
      merged.sort(compareByAbsDeltaDesc('retained'));
      rows = merged;
      dataSource = new InMemoryDataSource(merged);
      publishDiffRows('arrays', merged);
    } catch (err) {
      if (isStale()) return;
      error = err instanceof Error ? err.message : String(err);
      console.error('Arrays diff load failed:', err);
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
          title: `Failed to compute Arrays diff: ${error}`,
          fillHeight: true,
        });
      }
      if (!rows || !dataSource) {
        return m(EmptyState, {
          icon: 'data_array',
          title: 'No primitive arrays to diff',
          fillHeight: true,
        });
      }

      const size = {
        field: 'retained',
        title: 'Retained',
        kind: 'size' as const,
      };
      const count = {field: 'cnt', title: 'Count', kind: 'count' as const};
      const schema = buildSizeCountSchema({
        keyTitle: 'Array hash',
        keyRenderer: (value) =>
          m(
            'button',
            {
              class: 'pf-hde-link pf-hde-mono',
              onclick: () =>
                navigate('arrays', {arrayHash: String(value ?? '')}),
            },
            String(value ?? ''),
          ),
        size,
        count,
      });

      const initialColumns = buildSizeCountInitialColumns({size, count});

      return m('div', {class: 'pf-hde-view-content'}, [
        m('h2', {class: 'pf-hde-view-heading'}, [
          'Arrays diff ',
          m(
            'span',
            {class: 'pf-hde-muted'},
            `(${rows.length.toLocaleString()} hashes)`,
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

export default ArraysDiffView;
