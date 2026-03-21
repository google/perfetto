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
  countRenderer,
  shortClassName,
  DOMINATOR_TREE_PREAMBLE,
  REACHABLE_PREAMBLE,
  ReachableToggle,
} from '../components';

export interface ObjectsParams {
  className: string;
  heap: string | null;
}

interface ObjectsViewAttrs {
  engine: Engine;
  navigate: NavFn;
  params: ObjectsParams;
}

function objectsQuery(
  cls: string,
  heap: string | null,
  showReachable: boolean,
): string {
  const escaped = cls.replace(/'/g, "''");
  const heapFilter = heap
    ? `AND o.heap_type = '${heap.replace(/'/g, "''")}'`
    : '';
  const reachableJoin = showReachable
    ? 'LEFT JOIN _heap_graph_object_tree_aggregation a ON a.id = o.id'
    : '';
  const reachableCols = showReachable
    ? `,
      a.cumulative_size AS reachable_size,
      a.cumulative_native_size AS reachable_native,
      a.cumulative_count AS reachable_count`
    : '';
  return `
    SELECT
      o.id,
      ifnull(c.deobfuscated_name, c.name) AS cls,
      o.self_size,
      o.native_size,
      ifnull(d.dominated_size_bytes, o.self_size) AS retained,
      ifnull(d.dominated_native_size_bytes, o.native_size) AS retained_native,
      ifnull(d.dominated_obj_count, 1) AS retained_count,
      ifnull(o.heap_type, 'default') AS heap,
      od.value_string AS str${reachableCols}
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    LEFT JOIN heap_graph_object_data od ON od.object_id = o.id
    ${reachableJoin}
    WHERE o.reachable != 0
      AND (c.name = '${escaped}' OR c.deobfuscated_name = '${escaped}')
      ${heapFilter}
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
      retained_count: {
        title: 'Retained Count',
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
        title: 'Reachable Count',
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

function ObjectsView(): m.Component<ObjectsViewAttrs> {
  let dataSource: SQLDataSource | null = null;
  let prevClassName: string | undefined;
  let prevHeap: string | null | undefined;
  let showReachable = false;

  function createDataSource(engine: Engine, cls: string, heap: string | null) {
    dataSource = new SQLDataSource({
      engine,
      sqlSchema: createSimpleSchema(objectsQuery(cls, heap, showReachable)),
      rootSchemaName: 'query',
      preamble: showReachable ? REACHABLE_PREAMBLE : DOMINATOR_TREE_PREAMBLE,
    });
  }

  return {
    oninit(vnode) {
      const cls = vnode.attrs.params.className ?? '';
      const heap = vnode.attrs.params.heap ?? null;
      prevClassName = cls;
      prevHeap = heap;
      createDataSource(vnode.attrs.engine, cls, heap);
    },
    onupdate(vnode) {
      const cls = vnode.attrs.params.className ?? '';
      const heap = vnode.attrs.params.heap ?? null;
      if (cls !== prevClassName || heap !== prevHeap) {
        prevClassName = cls;
        prevHeap = heap;
        createDataSource(vnode.attrs.engine, cls, heap);
      }
    },
    view(vnode) {
      const {params, navigate} = vnode.attrs;
      const className = params.className ?? '';
      const heap = params.heap ?? null;

      if (!dataSource) return null;

      return m('div', {class: 'ah-view-content'}, [
        m('div', {key: 'heading', class: 'ah-view-heading-row'}, [
          m('h2', {class: 'ah-view-heading'}, 'Instances'),
          m(ReachableToggle, {
            checked: showReachable,
            onchange: (v: boolean) => {
              showReachable = v;
              createDataSource(vnode.attrs.engine, className, heap);
            },
          }),
        ]),
        m('div', {key: 'card', class: 'ah-card--compact ah-mb-3'}, [
          m('div', {class: 'ah-info-grid--compact'}, [
            m('span', {class: 'ah-info-grid__label'}, 'Class:'),
            m('span', {class: 'ah-mono'}, className),
            ...(heap
              ? [
                  m('span', {class: 'ah-info-grid__label'}, 'Heap:'),
                  m('span', heap),
                ]
              : []),
          ]),
        ]),
        m(DataGrid, {
          key: String(showReachable),
          schema: makeUiSchema(navigate),
          rootSchema: 'query',
          data: dataSource,
          fillHeight: true,
          initialColumns: [
            {id: 'self_size', field: 'self_size'},
            {id: 'native_size', field: 'native_size'},
            {id: 'retained', field: 'retained'},
            {id: 'retained_native', field: 'retained_native'},
            {id: 'retained_count', field: 'retained_count'},
            ...(showReachable
              ? [
                  {id: 'reachable_size', field: 'reachable_size'},
                  {id: 'reachable_native', field: 'reachable_native'},
                  {id: 'reachable_count', field: 'reachable_count'},
                ]
              : []),
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

export default ObjectsView;
