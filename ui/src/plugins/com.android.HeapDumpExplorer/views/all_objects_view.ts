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
import {
  type NavFn,
  sizeRenderer,
  shortClassName,
  DOMINATOR_TREE_PREAMBLE,
} from '../components';

interface AllObjectsViewAttrs {
  engine: Engine;
  navigate: NavFn;
}

const ALL_OBJECTS_QUERY = `
  SELECT
    o.id,
    ifnull(c.deobfuscated_name, c.name) AS cls,
    o.self_size,
    o.native_size,
    ifnull(d.dominated_size_bytes, o.self_size) AS retained,
    ifnull(d.dominated_native_size_bytes, o.native_size) AS retained_native,
    ifnull(o.heap_type, 'default') AS heap,
    od.value_string AS str
  FROM heap_graph_object o
  JOIN heap_graph_class c ON o.type_id = c.id
  LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
  LEFT JOIN heap_graph_object_data od ON od.object_id = o.id
  WHERE o.reachable != 0
`;

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
                class: 'ah-link',
                onclick: () =>
                  navigate('object', {id, label: str ? `"${str}"` : display}),
              },
              display,
            ),
            str
              ? m(
                  'span',
                  {class: 'ah-str-badge'},
                  ` "${str.length > 40 ? str.slice(0, 40) + '\u2026' : str}"`,
                )
              : null,
          ]);
        },
      },
      self_size: {
        title: 'Shallow',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      native_size: {
        title: 'Native',
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

  return {
    oninit(vnode) {
      dataSource = new SQLDataSource({
        engine: vnode.attrs.engine,
        sqlSchema: createSimpleSchema(ALL_OBJECTS_QUERY),
        rootSchemaName: 'query',
        preamble: DOMINATOR_TREE_PREAMBLE,
      });
    },
    view(vnode) {
      const {navigate} = vnode.attrs;

      if (!dataSource) return null;

      return m('div', {class: 'ah-view-content'}, [
        m('h2', {class: 'ah-view-heading'}, 'Objects'),
        m(DataGrid, {
          schema: makeUiSchema(navigate),
          rootSchema: 'query',
          data: dataSource,
          fillHeight: true,
          initialColumns: [
            {id: 'retained', field: 'retained', sort: 'DESC' as const},
            {id: 'retained_native', field: 'retained_native'},
            {id: 'self_size', field: 'self_size'},
            {id: 'native_size', field: 'native_size'},
            {id: 'heap', field: 'heap'},
            {id: 'cls', field: 'cls'},
            {id: 'id', field: 'id'},
          ],
          showExportButton: true,
        }),
      ]);
    },
  };
}

export default AllObjectsView;
