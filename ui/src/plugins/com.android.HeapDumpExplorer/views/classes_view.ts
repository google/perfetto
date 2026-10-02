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
import {Memo} from '../../../base/memo';
import type {Engine} from '../../../trace_processor/engine';
import type {SqlValue} from '../../../trace_processor/query_result';
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import {SQLDataSource} from '../../../components/widgets/datagrid/sql_data_source';
import type {Filter} from '../../../components/widgets/datagrid/model';
import {
  sizeRenderer,
  countRenderer,
  RowCounter,
  COL_INFO,
  colHeader,
} from '../components';
import * as queries from '../queries';
import type {ColumnSchema} from '../../../components/widgets/datagrid/datagrid_schema';
import {DetailsShell} from '../../../widgets/details_shell';
import {HdeAnchor} from '../nav';
import {AsyncMemo} from '../../../base/async_memo';

interface ClassesViewAttrs {
  readonly engine: Engine;
  readonly activeDump: queries.HeapDump;
  readonly rootClass?: string;
}

const PREAMBLE =
  'INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_class_aggregation';

function buildQuery(activeDump: queries.HeapDump): string {
  return `
    SELECT
      type_name AS cls,
      reachable_obj_count AS cnt,
      reachable_size_bytes AS shallow,
      reachable_native_size_bytes AS native_shallow,
      dominated_size_bytes AS retained,
      dominated_native_size_bytes AS retained_native,
      dominated_obj_count AS retained_count
    FROM android_heap_graph_class_aggregation a
    WHERE a.reachable_obj_count > 0 AND ${queries.dumpFilterSql(activeDump, 'a')}
  `;
}

function makeUiSchema(dump: queries.HeapDump): ColumnSchema {
  return {
    cls: {
      title: 'Class',
      columnType: 'text',
      cellRenderer: (value: SqlValue) =>
        m(
          HdeAnchor,
          {dump, to: {view: 'objects', cls: String(value)}},
          String(value),
        ),
    },
    cnt: {
      title: 'Count',
      columnType: 'quantitative',
      cellRenderer: countRenderer,
    },
    shallow: {
      title: colHeader('Shallow', COL_INFO.shallow),
      titleString: 'Shallow',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    native_shallow: {
      title: colHeader('Shallow Native', COL_INFO.shallowNative),
      titleString: 'Shallow Native',
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
  };
}

export function ClassesView({
  attrs: {engine},
}: m.Vnode<ClassesViewAttrs>): m.Component<ClassesViewAttrs> {
  const datasourceMemo = new Memo<SQLDataSource>();
  const counter = new RowCounter(engine, PREAMBLE);
  // Subclass names of the URL's root class, which the grid is filtered to.
  const subclassesMemo = new AsyncMemo<string[]>();

  return {
    onremove() {
      counter.dispose();
      subclassesMemo.dispose();
      datasourceMemo.dispose();
    },
    view({attrs}) {
      const {activeDump, rootClass} = attrs;
      const query = buildQuery(activeDump);
      const datasource = datasourceMemo.use({
        key: {query},
        compute: () =>
          new SQLDataSource({
            engine,
            tableOrSubquery: query,
            preamble: PREAMBLE,
          }),
      });
      const names = rootClass
        ? subclassesMemo.use({
            key: {upid: activeDump.upid, ts: activeDump.ts, rootClass},
            compute: () =>
              queries.getSubclassNames(engine, activeDump, rootClass),
          }).data
        : undefined;
      const filters: Filter[] =
        names !== undefined && names.length > 0
          ? [{field: 'cls', op: 'in', value: names}]
          : [];

      return m(
        DetailsShell,
        {
          title: counter.heading('Classes', query, filters),
          fillHeight: true,
        },
        m(DataGrid, {
          schema: makeUiSchema(activeDump),
          data: datasource,
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
        }),
      );
    },
  };
}
