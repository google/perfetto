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
import {baselineDumpFilterSql, getActiveBaseline} from '../../baseline/state';
import {dumpFilterSql, getActiveDump} from '../../queries';

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

async function runQuery(engine: Engine, filterSql: string): Promise<Row[]> {
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
      const baselineFilter = baselineDumpFilterSql('o');
      const currentFilter = dumpFilterSql(undefined, 'o');
      const [b, c] = await Promise.all([
        runQuery(baselineEngine, baselineFilter),
        runQuery(currentEngine, currentFilter),
      ]);
      if (isStale()) return;
      const merged = mergeRows({
        baseline: b,
        current: c,
        keyOf: (r) => String(r.root_class ?? ''),
        numericFields: NUMERIC_FIELDS,
        primaryDeltaField: 'dominated_size_bytes',
      });
      merged.sort(compareByAbsDeltaDesc('dominated_size_bytes'));
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
        return m('div', {class: 'ah-loading'}, m(Spinner, {easing: true}));
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
              class: 'ah-link',
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

      return m('div', {class: 'ah-view-content'}, [
        m('h2', {class: 'ah-view-heading'}, [
          'Dominators diff ',
          m(
            'span',
            {class: 'ah-muted'},
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
