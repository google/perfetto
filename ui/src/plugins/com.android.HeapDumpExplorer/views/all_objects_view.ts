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
import type {Engine} from '../../../trace_processor/engine';
import type {SqlValue} from '../../../trace_processor/query_result';
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import {SQLDataSource} from '../../../components/widgets/datagrid/sql_data_source';
import {createSimpleSchema} from '../../../components/widgets/datagrid/sql_schema';
import type {SchemaRegistry} from '../../../components/widgets/datagrid/datagrid_schema';
import {fmtHex} from '../format';
import type {Filter} from '../../../components/widgets/datagrid/model';
import {
  type NavFn,
  sizeRenderer,
  countRenderer,
  shortClassName,
  SQL_PREAMBLE,
  RowCounter,
  COL_INFO,
  colHeader,
} from '../components';
import {dumpFilterSql, type HeapDump} from '../queries';

interface AllObjectsViewAttrs {
  readonly engine: Engine;
  readonly activeDump: HeapDump;
  readonly navigate: NavFn;
  readonly clearNavParam: (key: string) => void;
  readonly initialClass?: string;
}

function buildQuery(activeDump: HeapDump): string {
  return `
    SELECT
      base.*,
      a.cumulative_size AS reachable_size,
      a.cumulative_native_size AS reachable_native,
      a.cumulative_count AS reachable_count
    FROM (
      SELECT
        o.id,
        ifnull(c.deobfuscated_name, c.name) AS cls,
        o.self_size,
        o.native_size,
        ifnull(d.dominated_size_bytes, o.self_size) AS retained,
        ifnull(d.dominated_native_size_bytes, o.native_size) AS retained_native,
        ifnull(d.dominated_obj_count, 1) AS retained_count,
        ifnull(o.heap_type, 'default') AS heap,
        od.value_string AS str
      FROM heap_graph_object o
      JOIN heap_graph_class c ON o.type_id = c.id
      LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
      LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
      WHERE o.reachable != 0
        AND ${dumpFilterSql(activeDump, 'o')}
    ) base
    LEFT JOIN _heap_graph_object_tree_aggregation a ON a.id = base.id
  `;
}

function makeUiSchema(navigate: NavFn): SchemaRegistry {
  return {
    query: {
      id: {
        title: 'Object',
        columnType: 'identifier',
        cellRenderer: (value: SqlValue, row) => {
          const id = Number(value);
          const cls = String(row.cls ?? '');
          const display = `${shortClassName(cls)} ${fmtHex(id)}`;
          const str = row.str != null ? String(row.str) : null;
          return m('span', [
            m(
              'button',
              {
                class: 'pf-hde-link',
                onclick: () =>
                  navigate('object', {id, label: str ? `"${str}"` : display}),
              },
              display,
            ),
            str
              ? m(
                  'span',
                  {class: 'pf-hde-str-badge'},
                  ` "${str.length > 40 ? str.slice(0, 40) + '\u2026' : str}"`,
                )
              : null,
          ]);
        },
      },
      self_size: {
        title: colHeader('Shallow', COL_INFO.shallow),
        titleString: 'Shallow',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      native_size: {
        title: colHeader('Native', COL_INFO.shallowNative),
        titleString: 'Native',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      retained: {
        title: colHeader('Retained', COL_INFO.retained),
        titleString: 'Retained',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      retained_native: {
        title: colHeader('Retained Native', COL_INFO.retainedNative),
        titleString: 'Retained Native',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      retained_count: {
        title: colHeader('Retained #', COL_INFO.retainedCount),
        titleString: 'Retained #',
        columnType: 'quantitative',
        cellRenderer: countRenderer,
      },
      reachable_size: {
        title: colHeader('Reachable', COL_INFO.reachable),
        titleString: 'Reachable',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      reachable_native: {
        title: colHeader('Reachable Native', COL_INFO.reachableNative),
        titleString: 'Reachable Native',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      reachable_count: {
        title: colHeader('Reachable #', COL_INFO.reachableCount),
        titleString: 'Reachable #',
        columnType: 'quantitative',
        cellRenderer: countRenderer,
      },
      heap: {
        title: 'Heap',
        columnType: 'text',
      },
      cls: {
        title: 'Class',
        columnType: 'text',
      },
      str: {
        title: 'String Value',
        columnType: 'text',
      },
    },
  };
}

function AllObjectsView(): m.Component<AllObjectsViewAttrs> {
  let dataSource: SQLDataSource | null = null;
  const counter = new RowCounter();
  let filters: Filter[] = [];

  function applyNavFilter(
    cls: string | undefined,
    clearNavParam: (key: string) => void,
  ) {
    if (!cls) return;
    filters = [{field: 'cls', op: '=' as const, value: cls}];
    counter.onFiltersChanged(filters);
    clearNavParam('cls');
  }

  return {
    oninit(vnode) {
      const {engine, activeDump} = vnode.attrs;
      const query = buildQuery(activeDump);
      dataSource = new SQLDataSource({
        engine,
        sqlSchema: createSimpleSchema(query),
        rootSchemaName: 'query',
        preamble: SQL_PREAMBLE,
      });
      counter.init(engine, query, SQL_PREAMBLE);
      applyNavFilter(vnode.attrs.initialClass, vnode.attrs.clearNavParam);
    },
    onupdate(vnode) {
      applyNavFilter(vnode.attrs.initialClass, vnode.attrs.clearNavParam);
    },
    view(vnode) {
      const {navigate} = vnode.attrs;

      if (!dataSource) return null;

      return m('div', {class: 'pf-hde-view-content'}, [
        m('h2', {class: 'pf-hde-view-heading'}, counter.heading('Objects')),
        m(DataGrid, {
          schema: makeUiSchema(navigate),
          rootSchema: 'query',
          data: dataSource,
          fillHeight: true,
          initialColumns: [
            {id: 'id', field: 'id'},
            {id: 'cls', field: 'cls'},
            {id: 'retained', field: 'retained', sort: 'DESC' as const},
            {id: 'retained_native', field: 'retained_native'},
            {id: 'retained_count', field: 'retained_count'},
            {id: 'self_size', field: 'self_size'},
            {id: 'native_size', field: 'native_size'},
            {id: 'reachable_size', field: 'reachable_size'},
            {id: 'reachable_native', field: 'reachable_native'},
            {id: 'reachable_count', field: 'reachable_count'},
            {id: 'heap', field: 'heap'},
          ],
          filters,
          showExportButton: true,
          onFiltersChanged: (f) => {
            filters = [...f];
            counter.onFiltersChanged(f);
          },
        }),
      ]);
    },
  };
}

export default AllObjectsView;
