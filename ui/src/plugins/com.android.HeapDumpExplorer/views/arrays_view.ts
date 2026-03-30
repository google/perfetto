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
import {EmptyState} from '../../../widgets/empty_state';
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import {SQLDataSource} from '../../../components/widgets/datagrid/sql_data_source';
import {createSimpleSchema} from '../../../components/widgets/datagrid/sql_schema';
import type {SchemaRegistry} from '../../../components/widgets/datagrid/datagrid_schema';
import type {Filter} from '../../../components/widgets/datagrid/model';
import {fmtHex} from '../format';
import {
  type NavFn,
  sizeRenderer,
  countRenderer,
  shortClassName,
  RowCounter,
} from '../components';
import {clearNavParam} from '../nav_state';

const QUERY = `
  SELECT
    o.id,
    ifnull(c.deobfuscated_name, c.name) AS cls,
    o.self_size,
    o.native_size,
    od.array_element_count AS element_count,
    ifnull(o.heap_type, 'default') AS heap,
    CAST(od.array_data_hash AS TEXT) AS array_hash
  FROM heap_graph_object o
  JOIN heap_graph_class c ON o.type_id = c.id
  LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
  WHERE o.reachable != 0
    AND od.array_data_hash IS NOT NULL
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
      cls: {
        title: 'Class',
        columnType: 'text',
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
      element_count: {
        title: 'Elements',
        columnType: 'quantitative',
        cellRenderer: countRenderer,
      },
      heap: {
        title: 'Heap',
        columnType: 'text',
      },
      array_hash: {
        title: 'Content Hash',
        columnType: 'text',
      },
    },
  };
}

interface ArraysViewAttrs {
  readonly engine: Engine;
  readonly navigate: NavFn;
  readonly initialArrayHash?: string;
  readonly hasFieldValues?: boolean;
}

function ArraysView(): m.Component<ArraysViewAttrs> {
  let dataSource: SQLDataSource | null = null;
  const counter = new RowCounter();
  let filters: Filter[] = [];

  function applyNavFilter(ah: string | undefined) {
    if (!ah) return;
    filters = [{field: 'array_hash', op: '=' as const, value: ah}];
    counter.onFiltersChanged(filters);
    clearNavParam('arrayHash');
  }

  return {
    oninit(vnode) {
      const {engine} = vnode.attrs;
      dataSource = new SQLDataSource({
        engine,
        sqlSchema: createSimpleSchema(QUERY),
        rootSchemaName: 'query',
      });
      counter.init(engine, QUERY);
      applyNavFilter(vnode.attrs.initialArrayHash);
    },
    onupdate(vnode) {
      applyNavFilter(vnode.attrs.initialArrayHash);
    },
    view(vnode) {
      const {navigate} = vnode.attrs;
      if (vnode.attrs.hasFieldValues === false) {
        return m(EmptyState, {
          icon: 'data_array',
          title: 'Array data requires an ART heap dump (.hprof)',
          fillHeight: true,
        });
      }

      if (!dataSource) return null;

      return m('div', {class: 'ah-view-content'}, [
        m('h2', {class: 'ah-view-heading'}, counter.heading('Arrays')),
        m(DataGrid, {
          schema: makeUiSchema(navigate),
          rootSchema: 'query',
          data: dataSource,
          fillHeight: true,
          initialColumns: [
            {id: 'self_size', field: 'self_size'},
            {id: 'native_size', field: 'native_size'},
            {id: 'element_count', field: 'element_count'},
            {id: 'cls', field: 'cls'},
            {id: 'heap', field: 'heap'},
            {id: 'id', field: 'id'},
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

export default ArraysView;
