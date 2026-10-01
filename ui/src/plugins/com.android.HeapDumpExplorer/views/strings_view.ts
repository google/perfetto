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
import type {SqlValue, Row} from '../../../trace_processor/query_result';
import {Spinner} from '../../../widgets/spinner';
import {EmptyState} from '../../../widgets/empty_state';
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import {SQLDataSource} from '../../../components/widgets/datagrid/sql_data_source';
import type {ColumnSchema} from '../../../components/widgets/datagrid/datagrid_schema';
import type {StringListRow} from '../types';
import type {Filter} from '../../../components/widgets/datagrid/model';
import {
  sizeRenderer,
  countRenderer,
  SQL_PREAMBLE,
  RowCounter,
  COL_INFO,
  colHeader,
  fmtSize,
  fmtHex,
} from '../components';
import * as queries from '../queries';
import {DetailsShell} from '../../../widgets/details_shell';
import {AsyncMemo} from '../../../base/async_memo';
import {type DumpRef, HdeAnchor} from '../nav';

function buildQuery(activeDump: queries.HeapDump): string {
  return `
    SELECT base.*,
      a.cumulative_size AS reachable_size,
      a.cumulative_native_size AS reachable_native,
      a.cumulative_count AS reachable_count
    FROM (
      SELECT
        o.id,
        od.value_string AS value,
        LENGTH(od.value_string) AS len,
        o.self_size,
        ifnull(d.dominated_size_bytes, o.self_size)
          + ifnull(d.dominated_native_size_bytes, o.native_size) AS retained,
        ifnull(o.heap_type, 'default') AS heap
      FROM heap_graph_object o
      JOIN heap_graph_class c ON o.type_id = c.id
      LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
      LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
      WHERE o.reachable != 0
        AND ${queries.dumpFilterSql(activeDump, 'o')}
        AND od.value_string IS NOT NULL
        AND (c.name = 'java.lang.String'
          OR c.deobfuscated_name = 'java.lang.String')
    ) base
    LEFT JOIN _heap_graph_object_tree_aggregation a ON a.id = base.id
  `;
}

function makeUiSchema(dump: DumpRef): ColumnSchema {
  return {
    id: {
      title: 'Object',
      columnType: 'identifier',
      cellRenderer: (value: SqlValue, row) => {
        const id = Number(value);
        const str = row.value != null ? String(row.value) : null;
        const display = `String ${fmtHex(id)}`;
        return m(
          HdeAnchor,
          {
            class: 'pf-hde-str-color',
            dump,
            to: {view: 'object', id},
          },
          m(
            'span',
            {
              class: 'pf-hde-mono pf-hde-break-all',
            },
            str
              ? '"' +
                  (str.length > 300 ? str.slice(0, 300) + '\u2026' : str) +
                  '"'
              : display,
          ),
        );
      },
    },
    retained: {
      title: colHeader('Retained', COL_INFO.retained),
      titleString: 'Retained',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    len: {
      title: 'Length',
      columnType: 'quantitative',
      cellRenderer: countRenderer,
    },
    heap: {
      title: 'Heap',
      columnType: 'text',
    },
    value: {
      title: 'Value',
      columnType: 'text',
    },
    self_size: {
      title: colHeader('Shallow', COL_INFO.shallow),
      titleString: 'Shallow',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    reachable_size: {
      title: colHeader('Reachable', COL_INFO.reachable),
      titleString: 'Reachable',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    reachable_native: {
      title: colHeader('Reachable native', COL_INFO.reachableNative),
      titleString: 'Reachable native',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    reachable_count: {
      title: colHeader('Reachable count', COL_INFO.reachableCount),
      titleString: 'Reachable count',
      columnType: 'quantitative',
      cellRenderer: countRenderer,
    },
  };
}

const SUMMARY_SCHEMA: ColumnSchema = {
  property: {title: 'Property', columnType: 'text'},
  value: {title: 'Value', columnType: 'text'},
};

// --- StringsView -------------------------------------------------------------

interface StringsViewAttrs {
  readonly engine: Engine;
  readonly activeDump: queries.HeapDump;
  readonly q?: string;
  readonly hasFieldValues: boolean;
}

export function StringsView({
  attrs: {engine},
}: m.Vnode<StringsViewAttrs>): m.Component<StringsViewAttrs> {
  const datasourceMemo = new Memo<SQLDataSource>();
  const counter = new RowCounter(engine, SQL_PREAMBLE);
  const allRowsMemo = new AsyncMemo<readonly StringListRow[]>();

  return {
    onremove() {
      counter.dispose();
      datasourceMemo.dispose();
      allRowsMemo.dispose();
    },
    view(vnode) {
      const {activeDump, q} = vnode.attrs;
      const query = buildQuery(activeDump);
      const filters: Filter[] = q ? [{field: 'value', op: '=', value: q}] : [];

      const {isPending, data: allRows} = allRowsMemo.use({
        key: {upid: activeDump.upid, ts: activeDump.ts},
        compute: () => queries.getStringList(engine, activeDump),
      });

      if (isPending) {
        return m(
          DetailsShell,
          {title: 'Strings', fillHeight: true, className: 'pf-hde-tab--padded'},
          m('div', {class: 'pf-hde-loading'}, m(Spinner, {easing: true})),
        );
      }

      if (allRows.length === 0) {
        return m(
          DetailsShell,
          {title: 'Strings', fillHeight: true, className: 'pf-hde-tab--padded'},
          m(EmptyState, {
            icon: 'text_fields',
            title: !vnode.attrs.hasFieldValues
              ? 'String values require an ART heap dump (.hprof)'
              : 'No string data available',
            fillHeight: true,
          }),
        );
      }

      const totalRetained = allRows.reduce((s, r) => s + r.retainedSize, 0);
      const uniqueCount = (() => {
        const seen = new Set<string>();
        for (const r of allRows) seen.add(r.value);
        return seen.size;
      })();

      const summaryRows: Row[] = [
        {property: 'Total strings', value: allRows.length.toLocaleString()},
        {property: 'Unique values', value: uniqueCount.toLocaleString()},
        {property: 'Total retained', value: fmtSize(totalRetained)},
      ];

      const datasource = datasourceMemo.use({
        key: {query},
        compute: () =>
          new SQLDataSource({
            engine,
            tableOrSubquery: query,
            preamble: SQL_PREAMBLE,
          }),
      });

      return m(
        DetailsShell,
        {
          title: counter.heading('Strings', query, filters),
          fillHeight: true,
          className: 'pf-hde-tab--padded',
        },
        [
          m('div', {class: 'pf-hde-card pf-hde-mb-4 pf-hde-flex-none'}, [
            m(DataGrid, {
              schema: SUMMARY_SCHEMA,
              data: summaryRows,
              initialColumns: [
                {id: 'property', field: 'property'},
                {id: 'value', field: 'value'},
              ],
            }),
          ]),

          m(DataGrid, {
            schema: makeUiSchema(activeDump),
            data: datasource,
            fillHeight: true,
            initialColumns: [
              {id: 'id', field: 'id'},
              {id: 'value', field: 'value'},
              {id: 'retained', field: 'retained'},
              {id: 'reachable_size', field: 'reachable_size'},
              {id: 'reachable_native', field: 'reachable_native'},
              {id: 'reachable_count', field: 'reachable_count'},
              {id: 'len', field: 'len'},
              {id: 'heap', field: 'heap'},
            ],
            filters,
            showExportButton: true,
          }),
        ],
      );
    },
  };
}
