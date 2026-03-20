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
import {
  type NavFn,
  sizeRenderer,
  countRenderer,
  DOMINATOR_TREE_PREAMBLE,
} from '../components';

interface ClassesViewAttrs {
  engine: Engine;
  navigate: NavFn;
}

const ALLOC_QUERY = `
  SELECT
    ifnull(c.deobfuscated_name, c.name) AS cls,
    COUNT(*) AS cnt,
    SUM(o.self_size) AS shallow,
    SUM(o.native_size) AS native_shallow,
    SUM(ifnull(d.dominated_size_bytes, o.self_size)) AS retained,
    SUM(ifnull(d.dominated_native_size_bytes, o.native_size))
      AS retained_native,
    ifnull(o.heap_type, 'default') AS heap
  FROM heap_graph_object o
  JOIN heap_graph_class c ON o.type_id = c.id
  LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
  WHERE o.reachable != 0
  GROUP BY cls, heap
`;

function makeUiSchema(navigate: NavFn): SchemaRegistry {
  return {
    query: {
      cls: {
        title: 'Class',
        columnType: 'text',
        cellRenderer: (value: SqlValue, row) =>
          m(
            'button',
            {
              class: 'ah-link',
              onclick: () =>
                navigate('instances', {
                  className: String(value),
                  heap:
                    String(row.heap) === 'default' ? null : String(row.heap),
                }),
            },
            String(value),
          ),
      },
      cnt: {
        title: 'Count',
        columnType: 'quantitative',
        cellRenderer: countRenderer,
      },
      shallow: {
        title: 'Shallow',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      native_shallow: {
        title: 'Shallow Native',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      retained: {
        title: 'Retained',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      retained_native: {
        title: 'Retained Native',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      heap: {
        title: 'Heap',
        columnType: 'text',
      },
    },
  };
}

function ClassesView(): m.Component<ClassesViewAttrs> {
  let dataSource: SQLDataSource | null = null;

  return {
    oninit(vnode) {
      dataSource = new SQLDataSource({
        engine: vnode.attrs.engine,
        sqlSchema: createSimpleSchema(ALLOC_QUERY),
        rootSchemaName: 'query',
        preamble: DOMINATOR_TREE_PREAMBLE,
      });
    },
    view(vnode) {
      const {navigate} = vnode.attrs;

      if (!dataSource) return null;

      return m('div', {class: 'ah-view-content'}, [
        m('h2', {class: 'ah-view-heading'}, 'Classes'),
        m(DataGrid, {
          schema: makeUiSchema(navigate),
          rootSchema: 'query',
          data: dataSource,
          fillHeight: true,
          initialColumns: [
            {id: 'shallow', field: 'shallow'},
            {id: 'native_shallow', field: 'native_shallow'},
            {id: 'retained', field: 'retained', sort: 'DESC' as const},
            {id: 'retained_native', field: 'retained_native'},
            {id: 'cnt', field: 'cnt'},
            {id: 'heap', field: 'heap'},
            {id: 'cls', field: 'cls'},
          ],
          showExportButton: true,
        }),
      ]);
    },
  };
}

export default ClassesView;
