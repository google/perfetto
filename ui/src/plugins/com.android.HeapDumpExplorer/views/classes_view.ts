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
  ReachableToggle,
  DOMINATOR_TREE_PREAMBLE,
  REACHABLE_PREAMBLE,
} from '../components';

interface ClassesViewAttrs {
  engine: Engine;
  navigate: NavFn;
}

function allocQuery(showReachable: boolean): string {
  const reachableSelect = showReachable
    ? `,
    SUM(a.cumulative_size) AS reachable_size,
    SUM(a.cumulative_native_size) AS reachable_native,
    SUM(a.cumulative_count) AS reachable_count`
    : '';
  const reachableJoin = showReachable
    ? 'LEFT JOIN _heap_graph_object_tree_aggregation a ON a.id = o.id'
    : '';
  return `
    SELECT
      ifnull(c.deobfuscated_name, c.name) AS cls,
      COUNT(*) AS cnt,
      SUM(o.self_size) AS shallow,
      SUM(o.native_size) AS native_shallow,
      SUM(ifnull(d.dominated_size_bytes, o.self_size)) AS retained,
      SUM(ifnull(d.dominated_native_size_bytes, o.native_size))
        AS retained_native,
      SUM(ifnull(d.dominated_obj_count, 1)) AS retained_count,
      ifnull(o.heap_type, 'default') AS heap${reachableSelect}
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    ${reachableJoin}
    WHERE o.reachable != 0
    GROUP BY cls, heap
  `;
}

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
      retained_count: {
        title: 'Retained #',
        columnType: 'quantitative',
        cellRenderer: countRenderer,
      },
      reachable_size: {
        title: 'Reachable',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      reachable_native: {
        title: 'Reachable Native',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      reachable_count: {
        title: 'Reachable #',
        columnType: 'quantitative',
        cellRenderer: countRenderer,
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
  let showReachable = false;

  function createDataSource(engine: Engine, reachable: boolean) {
    dataSource = new SQLDataSource({
      engine,
      sqlSchema: createSimpleSchema(allocQuery(reachable)),
      rootSchemaName: 'query',
      preamble: reachable ? REACHABLE_PREAMBLE : DOMINATOR_TREE_PREAMBLE,
    });
  }

  return {
    oninit(vnode) {
      createDataSource(vnode.attrs.engine, showReachable);
    },
    view(vnode) {
      const {engine, navigate} = vnode.attrs;

      if (!dataSource) return null;

      const initialColumns = [
        {id: 'shallow', field: 'shallow'},
        {id: 'native_shallow', field: 'native_shallow'},
        {id: 'retained', field: 'retained', sort: 'DESC' as const},
        {id: 'retained_native', field: 'retained_native'},
        {id: 'retained_count', field: 'retained_count'},
        {id: 'cnt', field: 'cnt'},
        ...(showReachable
          ? [
              {id: 'reachable_size', field: 'reachable_size'},
              {id: 'reachable_native', field: 'reachable_native'},
              {id: 'reachable_count', field: 'reachable_count'},
            ]
          : []),
        {id: 'heap', field: 'heap'},
        {id: 'cls', field: 'cls'},
      ];

      return m('div', {class: 'ah-view-content'}, [
        m('div', {key: 'heading', class: 'ah-heading-row'}, [
          m('h2', {class: 'ah-view-heading'}, 'Classes'),
          m(ReachableToggle, {
            checked: showReachable,
            onchange: (v: boolean) => {
              showReachable = v;
              createDataSource(engine, showReachable);
              m.redraw();
            },
          }),
        ]),
        m(DataGrid, {
          key: String(showReachable),
          schema: makeUiSchema(navigate),
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

export default ClassesView;
