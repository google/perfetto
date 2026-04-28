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
import type {Filter} from '../../../components/widgets/datagrid/model';
import {
  type NavFn,
  sizeRenderer,
  countRenderer,
  RowCounter,
} from '../components';
import {clearNavParam} from '../nav_state';
import * as queries from '../queries';

interface ClassesViewAttrs {
  readonly engine: Engine;
  readonly navigate: NavFn;
  readonly initialRootClass?: string;
}

const PREAMBLE =
  'INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_class_aggregation';

const QUERY = `
  SELECT
    type_name AS cls,
    reachable_obj_count AS cnt,
    reachable_size_bytes AS shallow,
    reachable_native_size_bytes AS native_shallow,
    dominated_size_bytes AS retained,
    dominated_native_size_bytes AS retained_native,
    dominated_obj_count AS retained_count
  FROM android_heap_graph_class_aggregation
  WHERE reachable_obj_count > 0
`;

function makeUiSchema(navigate: NavFn): SchemaRegistry {
  return {
    query: {
      cls: {
        title: 'Class',
        columnType: 'text',
        cellRenderer: (value: SqlValue) =>
          m(
            'button',
            {
              class: 'ah-link',
              onclick: () => navigate('objects', {cls: String(value)}),
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
    },
  };
}

function ClassesView(): m.Component<ClassesViewAttrs> {
  let dataSource: SQLDataSource | null = null;
  const counter = new RowCounter();
  let filters: Filter[] = [];

  async function applyNavFilter(engine: Engine, root: string | undefined) {
    if (!root) return;
    clearNavParam('rootClass');
    const names = await queries.getSubclassNames(engine, root);
    if (names.length === 0) return;
    filters = [{field: 'cls', op: 'in' as const, value: names}];
    counter.onFiltersChanged(filters);
    m.redraw();
  }

  return {
    oninit(vnode) {
      const {engine} = vnode.attrs;
      dataSource = new SQLDataSource({
        engine,
        sqlSchema: createSimpleSchema(QUERY),
        rootSchemaName: 'query',
        preamble: PREAMBLE,
      });
      counter.init(engine, QUERY, PREAMBLE);
      applyNavFilter(engine, vnode.attrs.initialRootClass).catch(console.error);
    },
    onupdate(vnode) {
      applyNavFilter(vnode.attrs.engine, vnode.attrs.initialRootClass).catch(
        console.error,
      );
    },
    view(vnode) {
      const {navigate} = vnode.attrs;

      if (!dataSource) return null;

      return m('div', {class: 'ah-view-content'}, [
        m('h2', {class: 'ah-view-heading'}, counter.heading('Classes')),
        m(DataGrid, {
          schema: makeUiSchema(navigate),
          rootSchema: 'query',
          data: dataSource,
          fillHeight: true,
          initialColumns: [
            {id: 'cls', field: 'cls'},
            {id: 'cnt', field: 'cnt'},
            {id: 'shallow', field: 'shallow'},
            {id: 'native_shallow', field: 'native_shallow'},
            {id: 'retained', field: 'retained', sort: 'DESC' as const},
            {id: 'retained_native', field: 'retained_native'},
            {id: 'retained_count', field: 'retained_count'},
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

export default ClassesView;
