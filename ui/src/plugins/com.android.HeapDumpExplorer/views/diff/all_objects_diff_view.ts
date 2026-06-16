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

// Per-class instance diff. Loads all instances of `cls` from both engines,
// pairs them via the AHAT-style bucket-key + retained-sort-zip in
// object_pairing.ts, then renders a paired DataGrid. Without `cls` shows
// an empty state — global instance pairing is too expensive to compute on
// the main thread, and class scope is what every navigation entry point
// (Classes, Dominators) already provides.

import m from 'mithril';
import {Spinner} from '../../../../widgets/spinner';
import {EmptyState} from '../../../../widgets/empty_state';
import type {Engine} from '../../../../trace_processor/engine';
import {
  NUM,
  NUM_NULL,
  STR_NULL,
  type SqlValue,
} from '../../../../trace_processor/query_result';
import {DataGrid} from '../../../../components/widgets/datagrid/datagrid';
import {InMemoryDataSource} from '../../../../components/widgets/datagrid/in_memory_data_source';
import type {
  CellRenderResult,
  CellRenderer,
  ColumnDef,
  SchemaRegistry,
} from '../../../../components/widgets/datagrid/datagrid_schema';
import {type NavFn, shortClassName, SQL_PREAMBLE} from '../../components';
import {fmtHex} from '../../format';
import {
  deltaSizeRenderer,
  sideSizeRenderer,
  sideCountRenderer,
  statusRenderer,
} from '../../diff/diff_schemas';
import {
  pairObjects,
  type ObjectPairRow,
  type ObjectRowRaw,
} from '../../diff/object_pairing';
import {baselineDumpFilterSql, getActiveBaseline} from '../../baseline/state';
import {dumpFilterSql, getActiveDump} from '../../queries';

interface AllObjectsDiffViewAttrs {
  readonly currentEngine: Engine;
  readonly baselineEngine: Engine;
  readonly cls: string | undefined;
  readonly navigate: NavFn;
}

const ITER_SPEC = {
  id: NUM,
  cls: STR_NULL,
  heap_type: STR_NULL,
  self_size: NUM,
  native_size: NUM,
  retained: NUM_NULL,
  retained_native: NUM_NULL,
  retained_count: NUM_NULL,
  value_string: STR_NULL,
  array_len: NUM_NULL,
};

function sqlEsc(s: string): string {
  return s.replace(/'/g, "''");
}

function buildQuery(filterSql: string, cls: string): string {
  const escaped = sqlEsc(cls);
  return `
    SELECT
      o.id AS id,
      coalesce(c.deobfuscated_name, c.name) AS cls,
      o.heap_type AS heap_type,
      o.self_size AS self_size,
      o.native_size AS native_size,
      coalesce(d.dominated_size_bytes, o.self_size) AS retained,
      coalesce(d.dominated_native_size_bytes, o.native_size) AS retained_native,
      coalesce(d.dominated_obj_count, 1) AS retained_count,
      od.value_string AS value_string,
      od.array_element_count AS array_len
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
    WHERE o.reachable != 0
      AND ${filterSql}
      AND (c.name = '${escaped}' OR c.deobfuscated_name = '${escaped}')
  `;
}

async function fetchObjectsForClass(
  engine: Engine,
  filterSql: string,
  cls: string,
): Promise<ObjectRowRaw[]> {
  await engine.query(SQL_PREAMBLE);
  const res = await engine.query(buildQuery(filterSql, cls));
  const out: ObjectRowRaw[] = [];
  for (const it = res.iter(ITER_SPEC); it.valid(); it.next()) {
    if (it.cls === null) continue;
    out.push({
      id: Number(it.id),
      className: it.cls,
      heapType: it.heap_type,
      valueString: it.value_string,
      arrayLength: it.array_len,
      shallow: it.self_size,
      shallowNative: it.native_size,
      retained: it.retained ?? it.self_size,
      retainedNative: it.retained_native ?? it.native_size,
      retainedCount: it.retained_count ?? 1,
    });
  }
  return out;
}

function pairToGridRow(p: ObjectPairRow): Record<string, SqlValue> {
  // Choose a stable display id: the current id if present, else the
  // baseline id. The Object column renderer reads c_id / b_id directly to
  // wire its click target.
  return {
    key: p.key,
    status: p.status,
    cls: p.className,
    heap: p.heapType,
    str: p.valueString,
    c_id: p.c_id,
    b_id: p.b_id,
    delta_retained: p.delta_retained,
    c_retained: p.c_retained,
    b_retained: p.b_retained,
    delta_shallow: p.delta_shallow,
    c_shallow: p.c_shallow,
    b_shallow: p.b_shallow,
    c_retained_count: p.c_retained_count,
    b_retained_count: p.b_retained_count,
  };
}

function buildSchema(navigate: NavFn): SchemaRegistry {
  const idRenderer: CellRenderer = (value) => {
    if (value === null || value === undefined) {
      return {
        content: m('span', {class: 'pf-hde-mono pf-hde-muted'}, '—'),
        align: 'right',
      } satisfies CellRenderResult;
    }
    return {
      content: m('span', {class: 'pf-hde-mono'}, fmtHex(Number(value))),
      align: 'right',
    } satisfies CellRenderResult;
  };

  // The Object column reads c_id / b_id from the same row to wire its
  // click target. InMemoryDataSource strips fields not declared in the
  // schema, so c_id and b_id MUST appear as columns below — even if we
  // also render them via this single composite cell.
  const objectRenderer: CellRenderer = (_value, row) => {
    const cls = String(row.cls ?? '');
    const cId = row.c_id == null ? null : Number(row.c_id);
    const bId = row.b_id == null ? null : Number(row.b_id);
    const str = row.str == null ? null : String(row.str);
    const displayId = cId ?? bId ?? 0;
    const display = `${shortClassName(cls)} ${fmtHex(displayId)}`;
    return {
      content: m('span', [
        m(
          'button',
          {
            class: 'pf-hde-link',
            onclick: () =>
              navigate('object', {
                id: cId ?? bId ?? 0,
                baselineId: bId,
                currentId: cId,
                label: str ? `"${truncate(str, 30)}"` : display,
              }),
          },
          display,
        ),
        str
          ? m('span', {class: 'pf-hde-str-badge'}, ` "${truncate(str, 40)}"`)
          : null,
      ]),
      align: 'left',
    } satisfies CellRenderResult;
  };

  const cols: Record<string, ColumnDef> = {
    cls: {title: 'Object', columnType: 'text', cellRenderer: objectRenderer},
    status: {
      title: 'Status',
      columnType: 'text',
      cellRenderer: statusRenderer,
    },
    delta_retained: {
      title: 'Δ Retained',
      columnType: 'quantitative',
      cellRenderer: deltaSizeRenderer,
    },
    b_retained: {
      title: 'Baseline Retained',
      columnType: 'quantitative',
      cellRenderer: sideSizeRenderer,
    },
    c_retained: {
      title: 'Current Retained',
      columnType: 'quantitative',
      cellRenderer: sideSizeRenderer,
    },
    delta_shallow: {
      title: 'Δ Shallow',
      columnType: 'quantitative',
      cellRenderer: deltaSizeRenderer,
    },
    b_shallow: {
      title: 'Baseline Shallow',
      columnType: 'quantitative',
      cellRenderer: sideSizeRenderer,
    },
    c_shallow: {
      title: 'Current Shallow',
      columnType: 'quantitative',
      cellRenderer: sideSizeRenderer,
    },
    b_retained_count: {
      title: 'Baseline Retained #',
      columnType: 'quantitative',
      cellRenderer: sideCountRenderer,
    },
    c_retained_count: {
      title: 'Current Retained #',
      columnType: 'quantitative',
      cellRenderer: sideCountRenderer,
    },
    heap: {title: 'Heap', columnType: 'text'},
    str: {title: 'String Value', columnType: 'text'},
    c_id: {
      title: 'Current id',
      columnType: 'identifier',
      cellRenderer: idRenderer,
    },
    b_id: {
      title: 'Baseline id',
      columnType: 'identifier',
      cellRenderer: idRenderer,
    },
  };
  return {query: cols};
}

function truncate(s: string, n: number): string {
  return s.length > n ? s.slice(0, n) + '…' : s;
}

function AllObjectsDiffView(): m.Component<AllObjectsDiffViewAttrs> {
  let rows: ObjectPairRow[] | null = null;
  let loading = false;
  let error: string | null = null;
  let dataSource: InMemoryDataSource | null = null;

  let lastCurrentEngine: Engine | null = null;
  let lastBaselineEngine: Engine | null = null;
  let lastCls: string | undefined = undefined;

  async function load(
    currentEngine: Engine,
    baselineEngine: Engine,
    cls: string,
  ) {
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
      const [baselineRows, currentRows] = await Promise.all([
        fetchObjectsForClass(baselineEngine, baselineFilter, cls),
        fetchObjectsForClass(currentEngine, currentFilter, cls),
      ]);
      if (isStale()) return;
      const paired = pairObjects(currentRows, baselineRows);
      // Default sort: largest |Δ retained| first.
      paired.sort(
        (a, b) => Math.abs(b.delta_retained) - Math.abs(a.delta_retained),
      );
      rows = paired;
      dataSource = new InMemoryDataSource(paired.map(pairToGridRow));
    } catch (err) {
      if (isStale()) return;
      error = err instanceof Error ? err.message : String(err);
      console.error('Objects diff load failed:', err);
    } finally {
      loading = false;
      m.redraw();
    }
  }

  function ensureLoaded(
    currentEngine: Engine,
    baselineEngine: Engine,
    cls: string | undefined,
  ) {
    if (
      currentEngine === lastCurrentEngine &&
      baselineEngine === lastBaselineEngine &&
      cls === lastCls
    ) {
      return;
    }
    lastCurrentEngine = currentEngine;
    lastBaselineEngine = baselineEngine;
    lastCls = cls;
    rows = null;
    dataSource = null;
    error = null;
    if (cls !== undefined) {
      load(currentEngine, baselineEngine, cls).catch(console.error);
    }
  }

  return {
    oninit(vnode) {
      ensureLoaded(
        vnode.attrs.currentEngine,
        vnode.attrs.baselineEngine,
        vnode.attrs.cls,
      );
    },
    onupdate(vnode) {
      ensureLoaded(
        vnode.attrs.currentEngine,
        vnode.attrs.baselineEngine,
        vnode.attrs.cls,
      );
    },
    view(vnode) {
      const {cls, navigate} = vnode.attrs;

      if (cls === undefined) {
        return m(
          EmptyState,
          {
            icon: 'filter_list',
            title: 'Pick a class to diff its instances',
            fillHeight: true,
          },
          m(
            'p',
            {class: 'pf-hde-muted'},
            'Open a class from the Classes or Dominators tab to see ' +
              'paired instance-level diffs.',
          ),
        );
      }

      if (loading && !rows) {
        return m('div', {class: 'pf-hde-loading'}, m(Spinner, {easing: true}));
      }
      if (error) {
        return m(EmptyState, {
          icon: 'error',
          title: `Failed to compute Objects diff: ${error}`,
          fillHeight: true,
        });
      }
      if (!rows || !dataSource) {
        return m(EmptyState, {
          icon: 'memory',
          title: `No instances of ${cls} in either dump`,
          fillHeight: true,
        });
      }

      const schema = buildSchema(navigate);
      return m('div', {class: 'pf-hde-view-content'}, [
        m('h2', {class: 'pf-hde-view-heading'}, [
          'Objects diff ',
          m(
            'span',
            {class: 'pf-hde-muted'},
            `(${cls} — ${rows.length.toLocaleString()} pairs)`,
          ),
        ]),
        m(DataGrid, {
          schema,
          rootSchema: 'query',
          data: dataSource,
          fillHeight: true,
          initialColumns: [
            {id: 'cls', field: 'cls'},
            {id: 'status', field: 'status'},
            {id: 'delta_retained', field: 'delta_retained', sort: 'DESC'},
            {id: 'b_retained', field: 'b_retained'},
            {id: 'c_retained', field: 'c_retained'},
            {id: 'delta_shallow', field: 'delta_shallow'},
            {id: 'b_shallow', field: 'b_shallow'},
            {id: 'c_shallow', field: 'c_shallow'},
            {id: 'b_retained_count', field: 'b_retained_count'},
            {id: 'c_retained_count', field: 'c_retained_count'},
            {id: 'heap', field: 'heap'},
            {id: 'str', field: 'str'},
            // Required for the Object column's click renderer to read both
            // ids — InMemoryDataSource projects rows down to declared
            // columns, so omitting these silently zeros them in the row.
            {id: 'c_id', field: 'c_id'},
            {id: 'b_id', field: 'b_id'},
          ],
          showExportButton: true,
        }),
      ]);
    },
  };
}

export default AllObjectsDiffView;
