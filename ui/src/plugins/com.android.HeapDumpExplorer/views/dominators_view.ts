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

interface DominatorsViewAttrs {
  engine: Engine;
  navigate: NavFn;
}

const DOMINATORS_QUERY = `
  SELECT
    o.id,
    ifnull(c.deobfuscated_name, c.name) AS cls,
    o.self_size,
    o.native_size,
    ifnull(d.dominated_size_bytes, o.self_size) AS retained,
    ifnull(d.dominated_native_size_bytes, o.native_size) AS retained_native,
    ifnull(o.heap_type, 'default') AS heap,
    o.root_type
  FROM heap_graph_dominator_tree d
  JOIN heap_graph_object o ON d.id = o.id
  JOIN heap_graph_class c ON o.type_id = c.id
  WHERE d.idom_id IS NULL
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
          return m(
            'button',
            {
              class: 'ah-link',
              onclick: () => navigate('object', {id, label: display}),
            },
            display,
          );
        },
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
      self_size: {
        title: 'Shallow',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      native_size: {
        title: 'Shallow Native',
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
      root_type: {
        title: 'Root Type',
        columnType: 'text',
      },
    },
  };
}

function DominatorsView(): m.Component<DominatorsViewAttrs> {
  let dataSource: SQLDataSource | null = null;

  return {
    oninit(vnode) {
      dataSource = new SQLDataSource({
        engine: vnode.attrs.engine,
        sqlSchema: createSimpleSchema(DOMINATORS_QUERY),
        rootSchemaName: 'query',
        preamble: DOMINATOR_TREE_PREAMBLE,
      });
    },
    view(vnode) {
      const {navigate} = vnode.attrs;

      if (!dataSource) return null;

      return m('div', {class: 'ah-view-content'}, [
        m('h2', {class: 'ah-view-heading'}, 'Dominators'),
        m(DataGrid, {
          schema: makeUiSchema(navigate),
          rootSchema: 'query',
          data: dataSource,
          fillHeight: true,
          initialColumns: [
            {id: 'retained', field: 'retained', sort: 'DESC' as const},
            {id: 'retained_native', field: 'retained_native'},
            {id: 'self_size', field: 'self_size'},
            {id: 'heap', field: 'heap'},
            {id: 'root_type', field: 'root_type'},
            {id: 'cls', field: 'cls'},
            {id: 'id', field: 'id'},
          ],
          showExportButton: true,
        }),
      ]);
    },
  };
}

export default DominatorsView;
