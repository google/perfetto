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

// Bitmaps diff: GROUP BY (width, height) per engine, JS outer-join.
// Pixel-content hashes rarely match across snapshots; dimensions answer
// "how many bitmaps of this size?".

import m from 'mithril';
import {Spinner} from '../../../../widgets/spinner';
import {EmptyState} from '../../../../widgets/empty_state';
import type {Engine} from '../../../../trace_processor/engine';
import {NUM, NUM_NULL} from '../../../../trace_processor/query_result';
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
      width,
      height,
      COUNT(*) AS cnt,
      SUM(retained) AS retained
    FROM (
      SELECT
        MAX(CASE WHEN f.field_name GLOB '*mWidth'
          THEN f.int_value END) AS width,
        MAX(CASE WHEN f.field_name GLOB '*mHeight'
          THEN f.int_value END) AS height,
        ifnull(d.dominated_size_bytes, o.self_size)
          + ifnull(d.dominated_native_size_bytes, o.native_size) AS retained
      FROM heap_graph_object o
      JOIN heap_graph_class c ON o.type_id = c.id
      LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
      LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
      LEFT JOIN heap_graph_primitive f ON f.field_set_id = od.field_set_id
      WHERE o.reachable != 0
        AND ${filterSql}
        AND (c.name = 'android.graphics.Bitmap'
          OR c.deobfuscated_name = 'android.graphics.Bitmap')
      GROUP BY o.id
    )
    WHERE width > 0 AND height > 0
    GROUP BY width, height
  `;
}

const ITER_SPEC = {
  width: NUM_NULL,
  height: NUM_NULL,
  cnt: NUM,
  retained: NUM,
};

interface BitmapsDiffViewAttrs {
  readonly currentEngine: Engine;
  readonly baselineEngine: Engine;
  readonly navigate: NavFn;
}

const NUMERIC_FIELDS = ['cnt', 'retained'];

async function runQuery(engine: Engine, filterSql: string): Promise<Row[]> {
  const res = await engine.query(buildQuery(filterSql));
  const out: Row[] = [];
  const it = res.iter(ITER_SPEC);
  for (; it.valid(); it.next()) {
    out.push({
      width: it.width,
      height: it.height,
      dimensions: `${it.width ?? 0} × ${it.height ?? 0}`,
      cnt: it.cnt,
      retained: it.retained,
    });
  }
  return dedupeByKey(out, (r) => String(r.dimensions ?? ''), NUMERIC_FIELDS);
}

function BitmapsDiffView(): m.Component<BitmapsDiffViewAttrs> {
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
      const currentFilter = dumpFilterSql(primarySnap, 'o');
      const [b, c] = await Promise.all([
        runQuery(baselineEngine, baselineFilter),
        runQuery(currentEngine, currentFilter),
      ]);
      if (isStale()) return;
      const merged = mergeRows({
        baseline: b,
        current: c,
        keyOf: (r) => String(r.dimensions ?? ''),
        numericFields: NUMERIC_FIELDS,
        primaryDeltaField: 'retained',
      });
      merged.sort(compareByAbsDeltaDesc('retained'));
      rows = merged;
      dataSource = new InMemoryDataSource(merged);
      publishDiffRows('bitmaps', merged);
    } catch (err) {
      if (isStale()) return;
      error = err instanceof Error ? err.message : String(err);
      console.error('Bitmaps diff load failed:', err);
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
          title: `Failed to compute Bitmaps diff: ${error}`,
          fillHeight: true,
        });
      }
      if (!rows || !dataSource) {
        return m(EmptyState, {
          icon: 'image',
          title: 'No bitmaps to diff',
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
        keyTitle: 'Dimensions',
        keyRenderer: (value) =>
          m(
            'button',
            {
              class: 'pf-hde-link pf-hde-mono',
              onclick: () => navigate('bitmaps'),
            },
            String(value ?? ''),
          ),
        size,
        count,
      });

      const initialColumns = buildSizeCountInitialColumns({size, count});

      return m('div', {class: 'pf-hde-view-content'}, [
        m('h2', {class: 'pf-hde-view-heading'}, [
          'Bitmaps diff ',
          m(
            'span',
            {class: 'pf-hde-muted'},
            `(${rows.length.toLocaleString()} dimension groups)`,
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

export default BitmapsDiffView;
