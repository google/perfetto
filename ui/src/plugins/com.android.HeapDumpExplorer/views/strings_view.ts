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
import type {SqlValue, Row} from '../../../trace_processor/query_result';
import {Spinner} from '../../../widgets/spinner';
import {EmptyState} from '../../../widgets/empty_state';
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import {SQLDataSource} from '../../../components/widgets/datagrid/sql_data_source';
import {createSimpleSchema} from '../../../components/widgets/datagrid/sql_schema';
import type {SchemaRegistry} from '../../../components/widgets/datagrid/datagrid_schema';
import type {StringListRow} from '../types';
import {fmtSize, fmtHex} from '../format';
import type {Filter} from '../../../components/widgets/datagrid/model';
import {
  type NavFn,
  sizeRenderer,
  countRenderer,
  SQL_PREAMBLE,
  RowCounter,
} from '../components';
import {clearNavParam} from '../nav_state';
import * as queries from '../queries';

const QUERY = `
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
      AND od.value_string IS NOT NULL
      AND (c.name = 'java.lang.String'
        OR c.deobfuscated_name = 'java.lang.String')
  ) base
  LEFT JOIN _heap_graph_object_tree_aggregation a ON a.id = base.id
`;

function makeUiSchema(navigate: NavFn): SchemaRegistry {
  return {
    query: {
      id: {
        title: 'Object',
        columnType: 'identifier',
        cellRenderer: (value: SqlValue, row) => {
          const id = Number(value);
          const str = row.value != null ? String(row.value) : null;
          const display = `String ${fmtHex(id)}`;
          return m(
            'button',
            {
              class: 'ah-link',
              onclick: () =>
                navigate('object', {
                  id,
                  label: str
                    ? `"${str.length > 40 ? str.slice(0, 40) + '\u2026' : str}"`
                    : display,
                }),
            },
            m(
              'span',
              {
                class: 'ah-mono ah-break-all ah-str-color',
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
        title: 'Retained',
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
        title: 'Shallow',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      reachable_size: {
        title: 'Reachable',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      reachable_native: {
        title: 'Reachable native',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      reachable_count: {
        title: 'Reachable count',
        columnType: 'quantitative',
        cellRenderer: countRenderer,
      },
    },
  };
}

const SUMMARY_SCHEMA: SchemaRegistry = {
  query: {
    property: {title: 'Property', columnType: 'text'},
    value: {title: 'Value', columnType: 'text'},
  },
};

// --- StringsView -------------------------------------------------------------

interface StringsViewAttrs {
  readonly engine: Engine;
  readonly navigate: NavFn;
  readonly initialQuery?: string;
  readonly hasFieldValues?: boolean;
}

function StringsView(): m.Component<StringsViewAttrs> {
  let allRows: StringListRow[] | null = null;
  let alive = true;
  let dataSource: SQLDataSource | null = null;
  const counter = new RowCounter();
  let filters: Filter[] = [];

  function applyNavFilter(q: string | undefined) {
    if (!q) return;
    filters = [{field: 'value', op: '=' as const, value: q}];
    counter.onFiltersChanged(filters);
    clearNavParam('q');
  }

  return {
    oninit(vnode) {
      const {engine} = vnode.attrs;
      dataSource = new SQLDataSource({
        engine,
        sqlSchema: createSimpleSchema(QUERY),
        rootSchemaName: 'query',
        preamble: SQL_PREAMBLE,
      });
      counter.init(engine, QUERY, SQL_PREAMBLE);
      applyNavFilter(vnode.attrs.initialQuery);
      queries
        .getStringList(vnode.attrs.engine)
        .then((r) => {
          if (!alive) return;
          allRows = r;
          m.redraw();
        })
        .catch(console.error);
    },
    onupdate(vnode) {
      applyNavFilter(vnode.attrs.initialQuery);
    },
    onremove() {
      alive = false;
    },
    view(vnode) {
      const {navigate} = vnode.attrs;

      if (!allRows) {
        return m('div', {class: 'ah-loading'}, m(Spinner, {easing: true}));
      }

      if (allRows.length === 0) {
        return m(EmptyState, {
          icon: 'text_fields',
          title:
            vnode.attrs.hasFieldValues === false
              ? 'String values require an ART heap dump (.hprof)'
              : 'No string data available',
          fillHeight: true,
        });
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

      return m('div', {class: 'ah-view-content'}, [
        m('h2', {class: 'ah-view-heading'}, counter.heading('Strings')),

        m('div', {class: 'ah-card ah-mb-4 ah-flex-none'}, [
          m(DataGrid, {
            schema: SUMMARY_SCHEMA,
            rootSchema: 'query',
            data: summaryRows,
            initialColumns: [
              {id: 'property', field: 'property'},
              {id: 'value', field: 'value'},
            ],
          }),
        ]),

        dataSource
          ? m(DataGrid, {
              schema: makeUiSchema(navigate),
              rootSchema: 'query',
              data: dataSource,
              fillHeight: true,
              initialColumns: [
                {id: 'retained', field: 'retained'},
                {id: 'reachable_size', field: 'reachable_size'},
                {id: 'reachable_native', field: 'reachable_native'},
                {id: 'reachable_count', field: 'reachable_count'},
                {id: 'len', field: 'len'},
                {id: 'value', field: 'value'},
                {id: 'heap', field: 'heap'},
                {id: 'id', field: 'id'},
              ],
              filters,
              showExportButton: true,
              onFiltersChanged: (f) => {
                filters = [...f];
                counter.onFiltersChanged(f);
              },
            })
          : null,
      ]);
    },
  };
}

export default StringsView;
