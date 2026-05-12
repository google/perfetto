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
import {baselineDumpFilterSql, getActiveBaseline} from '../../baseline/state';
import {dumpFilterSql, getActiveDump} from '../../queries';

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

async function fetchAll(engine: Engine, filterSql: string): Promise<Row[]> {
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
      const baselineFilter = baselineDumpFilterSql('a');
      const currentFilter = dumpFilterSql(undefined, 'a');
      const [baselineRows, currentRows] = await Promise.all([
        fetchAll(baselineEngine, baselineFilter),
        fetchAll(currentEngine, currentFilter),
      ]);
      if (isStale()) return;
      const merged = mergeRows({
        baseline: baselineRows,
        current: currentRows,
        keyOf: (r) => String(r.type_name ?? ''),
        numericFields: NUMERIC_FIELDS,
        primaryDeltaField: 'dominated_size_bytes',
      });
      merged.sort(compareByAbsDeltaDesc('dominated_size_bytes'));
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
        return m('div', {class: 'ah-loading'}, m(Spinner, {easing: true}));
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
              class: 'ah-link',
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

      return m('div', {class: 'ah-view-content'}, [
        m('h2', {class: 'ah-view-heading'}, [
          'Classes diff ',
          m(
            'span',
            {class: 'ah-muted'},
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
