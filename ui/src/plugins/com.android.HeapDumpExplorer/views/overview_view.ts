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
import type {SqlValue, Row} from '../../../trace_processor/query_result';
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import type {SchemaRegistry} from '../../../components/widgets/datagrid/datagrid_schema';
import type {OverviewData} from '../types';
import {fmtSize} from '../format';
import type {NavState} from '../nav_state';
import {type NavFn, sizeRenderer} from '../components';

const HEAP_SCHEMA: SchemaRegistry = {
  query: {
    heap: {
      title: 'Heap',
      columnType: 'text',
    },
    java_size: {
      title: 'Java Size',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    native_size: {
      title: 'Native Size',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    total_size: {
      title: 'Total Size',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
  },
};

const INFO_SCHEMA: SchemaRegistry = {
  query: {
    property: {
      title: 'Property',
      columnType: 'text',
    },
    value: {
      title: 'Value',
      columnType: 'text',
    },
  },
};

function makeDuplicateBitmapSchema(navigate: NavFn): SchemaRegistry {
  return {
    query: {
      dimensions: {
        title: 'Dimensions',
        columnType: 'text',
      },
      copies: {
        title: 'Copies',
        columnType: 'quantitative',
        cellRenderer: (value: SqlValue, row) =>
          m(
            'button',
            {
              class: 'ah-link',
              onclick: () =>
                navigate('bitmaps', {
                  filterKey: String(row.groupKey ?? ''),
                }),
            },
            String(value),
          ),
      },
      total_bytes: {
        title: 'Total',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      wasted_bytes: {
        title: 'Wasted',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
    },
  };
}

function makeDuplicateStringSchema(navigate: NavFn): SchemaRegistry {
  return {
    query: {
      value: {
        title: 'Value',
        columnType: 'text',
        cellRenderer: (value: SqlValue) =>
          m(
            'button',
            {
              class: 'ah-link ah-mono ah-break-all ah-str-color',
              onclick: () =>
                navigate('strings', {
                  q: String(value ?? ''),
                }),
            },
            '"' +
              (String(value ?? '').length > 200
                ? String(value).slice(0, 200) + '\u2026'
                : String(value ?? '')) +
              '"',
          ),
      },
      copies: {
        title: 'Copies',
        columnType: 'quantitative',
        cellRenderer: (value: SqlValue, row) =>
          m(
            'button',
            {
              class: 'ah-link',
              onclick: () => navigate('strings', {q: String(row.value ?? '')}),
            },
            String(value),
          ),
      },
      total_bytes: {
        title: 'Total',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      wasted_bytes: {
        title: 'Wasted',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
    },
  };
}

function renderDuplicateSection(
  title: string,
  groupCount: number,
  totalWasted: number,
  targetView: string,
  linkLabel: string,
  navigate: NavFn,
  schema: SchemaRegistry,
  data: Row[],
  columns: Array<{id: string; field: string}>,
): m.Children {
  return m('div', {class: 'ah-card ah-mt-4'}, [
    m('h3', {class: 'ah-sub-heading'}, title),
    m('p', {class: 'ah-desc'}, [
      groupCount +
        ' group' +
        (groupCount > 1 ? 's' : '') +
        ' detected, wasting ',
      m('span', {class: 'ah-mono ah-semibold'}, fmtSize(totalWasted)),
      '. ',
      m(
        'button',
        {
          class: 'ah-link--alt',
          onclick: () => navigate(targetView as NavState['view']),
        },
        linkLabel,
      ),
    ]),
    m(DataGrid, {
      schema,
      rootSchema: 'query',
      data,
      initialColumns: columns,
    }),
  ]);
}

interface OverviewViewAttrs {
  overview: OverviewData;
  navigate: NavFn;
}
function OverviewView(): m.Component<OverviewViewAttrs> {
  return {
    view(vnode) {
      const {overview, navigate} = vnode.attrs;
      const heapIndices: number[] = [];
      for (let i = 0; i < overview.heaps.length; i++) {
        const h = overview.heaps[i];
        if (h.java + h.native_ > 0) {
          heapIndices.push(i);
        }
      }
      const heaps = heapIndices.map((i) => overview.heaps[i]);
      const totalJava = heaps.reduce((a, h) => a + h.java, 0);
      const totalNative = heaps.reduce((a, h) => a + h.native_, 0);

      const heapRows: Row[] = [
        {
          heap: 'Total',
          java_size: totalJava,
          native_size: totalNative,
          total_size: totalJava + totalNative,
        },
        ...heaps.map((h) => ({
          heap: h.name,
          java_size: h.java,
          native_size: h.native_,
          total_size: h.java + h.native_,
        })),
      ];

      const infoRows: Row[] = [
        {
          property: 'Instances',
          value: overview.instanceCount.toLocaleString(),
        },
        {property: 'Heaps', value: heaps.map((h) => h.name).join(', ')},
      ];

      return m('div', {class: 'ah-view-scroll'}, [
        m('h2', {class: 'ah-view-heading'}, 'Overview'),

        m('div', {class: 'ah-card ah-mb-4'}, [
          m('h3', {class: 'ah-sub-heading'}, 'General Information'),
          m(DataGrid, {
            schema: INFO_SCHEMA,
            rootSchema: 'query',
            data: infoRows,
            initialColumns: [
              {id: 'property', field: 'property'},
              {id: 'value', field: 'value'},
            ],
          }),
        ]),
        m('div', {class: 'ah-card'}, [
          m('h3', {class: 'ah-sub-heading'}, 'Bytes Retained by Heap'),
          m(DataGrid, {
            schema: HEAP_SCHEMA,
            rootSchema: 'query',
            data: heapRows,
            initialColumns: [
              {id: 'heap', field: 'heap'},
              {id: 'java_size', field: 'java_size'},
              {id: 'native_size', field: 'native_size'},
              {id: 'total_size', field: 'total_size'},
            ],
          }),
        ]),
        overview.duplicateBitmaps && overview.duplicateBitmaps.length > 0
          ? renderDuplicateSection(
              'Duplicate Bitmaps',
              overview.duplicateBitmaps.length,
              overview.duplicateBitmaps.reduce((a, g) => a + g.wastedBytes, 0),
              'bitmaps',
              'View Bitmaps',
              navigate,
              makeDuplicateBitmapSchema(navigate),
              overview.duplicateBitmaps.map((g) => ({
                dimensions: `${g.width} \u00d7 ${g.height}`,
                groupKey: g.groupKey,
                copies: g.count,
                total_bytes: g.totalBytes,
                wasted_bytes: g.wastedBytes,
              })),
              [
                {id: 'dimensions', field: 'dimensions'},
                {id: 'groupKey', field: 'groupKey'},
                {id: 'copies', field: 'copies'},
                {id: 'total_bytes', field: 'total_bytes'},
                {id: 'wasted_bytes', field: 'wasted_bytes'},
              ],
            )
          : overview.hasFieldValues
            ? m(
                'div',
                {class: 'ah-card ah-mb-4'},
                m('p', {class: 'ah-muted'}, 'No duplicate bitmaps found.'),
              )
            : null,
        overview.duplicateStrings && overview.duplicateStrings.length > 0
          ? renderDuplicateSection(
              'Duplicate Strings',
              overview.duplicateStrings.length,
              overview.duplicateStrings.reduce((a, g) => a + g.wastedBytes, 0),
              'strings',
              'View Strings',
              navigate,
              makeDuplicateStringSchema(navigate),
              overview.duplicateStrings.map((g) => ({
                value: g.value,
                copies: g.count,
                total_bytes: g.totalBytes,
                wasted_bytes: g.wastedBytes,
              })),
              [
                {id: 'value', field: 'value'},
                {id: 'copies', field: 'copies'},
                {id: 'total_bytes', field: 'total_bytes'},
                {id: 'wasted_bytes', field: 'wasted_bytes'},
              ],
            )
          : overview.hasFieldValues
            ? m(
                'div',
                {class: 'ah-card ah-mb-4'},
                m('p', {class: 'ah-muted'}, 'No duplicate strings found.'),
              )
            : null,
      ]);
    },
  };
}

export default OverviewView;
