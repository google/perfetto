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
import {
  type NavFn,
  Section,
  sizeRenderer,
  countRenderer,
  DOMINATOR_TREE_PREAMBLE,
} from '../components';
import {computeDuplicates, type DuplicateGroup} from './strings_helpers';
import * as queries from '../queries';

const STRINGS_QUERY = `
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
  LEFT JOIN heap_graph_object_data od ON od.object_id = o.id
  LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
  WHERE o.reachable != 0
    AND od.value_string IS NOT NULL
    AND (c.name = 'java.lang.String'
      OR c.deobfuscated_name = 'java.lang.String')
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
                class: 'ah-mono ah-break-all',
                style: {color: 'var(--ah-badge-string)'},
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
    },
  };
}

// --- Duplicates DataGrid schema -----------------------------------------------

function duplicateGroupToRow(d: DuplicateGroup): Row {
  return {
    wasted: d.wastedBytes,
    count: d.count,
    value: d.value,
  };
}

const DUPLICATES_SCHEMA: SchemaRegistry = {
  query: {
    wasted: {
      title: 'Wasted',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    count: {
      title: 'Count',
      columnType: 'quantitative',
      cellRenderer: countRenderer,
    },
    value: {
      title: 'Value',
      columnType: 'text',
      cellRenderer: (value: SqlValue) =>
        m(
          'span',
          {
            class: 'ah-mono ah-break-all',
            style: {color: 'var(--ah-badge-string)'},
          },
          '"' +
            (String(value ?? '').length > 200
              ? String(value).slice(0, 200) + '\u2026'
              : String(value ?? '')) +
            '"',
        ),
    },
  },
};

// --- StringsView -------------------------------------------------------------

interface StringsViewAttrs {
  engine: Engine;
  navigate: NavFn;
  initialQuery?: string;
  hasFieldValues?: boolean;
}

function StringsView(): m.Component<StringsViewAttrs> {
  let allRows: StringListRow[] | null = null;
  let alive = true;
  let dataSource: SQLDataSource | null = null;

  return {
    oninit(vnode) {
      dataSource = new SQLDataSource({
        engine: vnode.attrs.engine,
        sqlSchema: createSimpleSchema(STRINGS_QUERY),
        rootSchemaName: 'query',
        preamble: DOMINATOR_TREE_PREAMBLE,
      });
      queries
        .getStringList(vnode.attrs.engine)
        .then((r) => {
          if (!alive) return;
          allRows = r;
          m.redraw();
        })
        .catch(console.error);
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

      const duplicates = computeDuplicates(allRows);
      const totalRetained = allRows.reduce((s, r) => s + r.retainedSize, 0);
      const totalWasted = duplicates.reduce((s, d) => s + d.wastedBytes, 0);
      const uniqueCount = (() => {
        const seen = new Set<string>();
        for (const r of allRows) seen.add(r.value);
        return seen.size;
      })();

      return m('div', {class: 'ah-view-content'}, [
        m('h2', {class: 'ah-view-heading'}, 'Strings'),

        m('div', {class: 'ah-card ah-mb-4', style: {flexShrink: '0'}}, [
          m('div', {class: 'ah-info-grid'}, [
            m('span', {class: 'ah-info-grid__label'}, 'Total strings:'),
            m('span', {class: 'ah-mono'}, allRows.length.toLocaleString()),
            m('span', {class: 'ah-info-grid__label'}, 'Unique values:'),
            m('span', {class: 'ah-mono'}, uniqueCount.toLocaleString()),
            m('span', {class: 'ah-info-grid__label'}, 'Duplicate groups:'),
            m(
              'span',
              {class: 'ah-mono'},
              duplicates.length > 0
                ? m(
                    'span',
                    {style: {color: 'var(--ah-badge-warning)'}},
                    duplicates.length.toLocaleString(),
                  )
                : '0',
            ),
            ...(totalWasted > 0
              ? [
                  m(
                    'span',
                    {class: 'ah-info-grid__label'},
                    'Wasted by duplicates:',
                  ),
                  m(
                    'span',
                    {
                      class: 'ah-mono',
                      style: {color: 'var(--ah-badge-warning)'},
                    },
                    fmtSize(totalWasted),
                  ),
                ]
              : []),
            m('span', {class: 'ah-info-grid__label'}, 'Total retained:'),
            m('span', {class: 'ah-mono'}, fmtSize(totalRetained)),
          ]),
        ]),

        duplicates.length > 0
          ? m('div', {class: 'ah-mb-4', style: {flexShrink: '0'}}, [
              m(
                Section,
                {
                  title: `Duplicate strings (${duplicates.length} groups, ${fmtSize(totalWasted)} wasted)`,
                  defaultOpen: false,
                },
                m(
                  'div',
                  {class: 'ah-subtable'},
                  m(DataGrid, {
                    schema: DUPLICATES_SCHEMA,
                    rootSchema: 'query',
                    data: duplicates.map(duplicateGroupToRow),
                    fillHeight: true,
                    initialColumns: [
                      {id: 'wasted', field: 'wasted'},
                      {id: 'count', field: 'count'},
                      {id: 'value', field: 'value'},
                    ],
                  }),
                ),
              ),
            ])
          : null,

        dataSource
          ? m(DataGrid, {
              schema: makeUiSchema(navigate),
              rootSchema: 'query',
              data: dataSource,
              fillHeight: true,
              initialColumns: [
                {id: 'retained', field: 'retained'},
                {id: 'len', field: 'len'},
                {id: 'value', field: 'value'},
                {id: 'heap', field: 'heap'},
                {id: 'id', field: 'id'},
              ],
              showExportButton: true,
            })
          : null,
      ]);
    },
  };
}

export default StringsView;
