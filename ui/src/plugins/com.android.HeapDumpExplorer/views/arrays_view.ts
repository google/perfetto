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
import {EmptyState} from '../../../widgets/empty_state';
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import {SQLDataSource} from '../../../components/widgets/datagrid/sql_data_source';
import type {ColumnSchema} from '../../../components/widgets/datagrid/datagrid_schema';
import type {Filter} from '../../../components/widgets/datagrid/model';
import {
  sizeRenderer,
  countRenderer,
  shortClassName,
  RowCounter,
  COL_INFO,
  colHeader,
  fmtHex,
} from '../components';
import * as queries from '../queries';
import {DetailsShell} from '../../../widgets/details_shell';
import {type DumpRef, HdeAnchor} from '../nav';

function buildQuery(activeDump: queries.HeapDump): string {
  return `
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
      AND ${queries.dumpFilterSql(activeDump, 'o')}
      AND od.array_data_hash IS NOT NULL
  `;
}

function makeUiSchema(dump: DumpRef): ColumnSchema {
  return {
    id: {
      title: 'Object',
      columnType: 'identifier',
      cellRenderer: (value: SqlValue, row) => {
        const id = Number(value);
        const cls = String(row.cls ?? '');
        const display = `${shortClassName(cls)} ${fmtHex(id)}`;
        return m(HdeAnchor, {dump, to: {view: 'object', id}}, display);
      },
    },
    cls: {
      title: 'Class',
      columnType: 'text',
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
  };
}

interface ArraysViewAttrs {
  readonly engine: Engine;
  readonly activeDump: queries.HeapDump;
  readonly arrayHash?: string;
  readonly hasFieldValues: boolean;
}

export function ArraysView({
  attrs: {engine},
}: m.Vnode<ArraysViewAttrs>): m.Component<ArraysViewAttrs> {
  const datasourceMemo = new Memo<SQLDataSource>();
  const counter = new RowCounter(engine);

  return {
    onremove() {
      counter.dispose();
      datasourceMemo.dispose();
    },
    view({attrs}) {
      const {activeDump, arrayHash} = attrs;
      if (!attrs.hasFieldValues) {
        return m(
          DetailsShell,
          {title: 'Arrays', fillHeight: true},
          m(EmptyState, {
            icon: 'data_array',
            title: 'Array data requires an ART heap dump (.hprof)',
            fillHeight: true,
          }),
        );
      }

      const query = buildQuery(activeDump);
      const datasource = datasourceMemo.use({
        key: {query},
        compute: () => new SQLDataSource({engine, tableOrSubquery: query}),
      });
      const filters: Filter[] = arrayHash
        ? [{field: 'array_hash', op: '=', value: arrayHash}]
        : [];

      return m(
        DetailsShell,
        {
          title: counter.heading('Arrays', query, filters),
          fillHeight: true,
        },
        m(DataGrid, {
          schema: makeUiSchema(activeDump),
          data: datasource,
          fillHeight: true,
          initialColumns: [
            {id: 'id', field: 'id'},
            {id: 'cls', field: 'cls'},
            {id: 'self_size', field: 'self_size'},
            {id: 'native_size', field: 'native_size'},
            {id: 'element_count', field: 'element_count'},
            {id: 'heap', field: 'heap'},
          ],
          filters,
          showExportButton: true,
        }),
      );
    },
  };
}
