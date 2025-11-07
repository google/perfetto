// Copyright (C) 2025 The Android Open Source Project
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
import {Grid, GridCell, GridHeaderCell, GridRow} from '../../../widgets/grid';
import {renderWidgetShowcase} from '../widgets_page_utils';
import {languages} from '../sample_data';

export function renderGrid(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Grid'),
      m('p', [
        'Grid is a purely presentational component for rendering tabular data with ',
        'virtual scrolling and column resizing. Unlike DataGrid, it provides no automatic ',
        'features like sorting or filtering - you must provide all content as GridCell and ',
        'GridHeaderCell components. It is useful as a replacement for simple HTML tables or ',
        'for building higher level grid experiences.',
      ]),
    ),

    renderWidgetShowcase({
      renderWidget: ({virtualize, wrap}) => {
        if (virtualize) {
          return m(VirtualGridDemo);
        } else {
          return renderSimpleGridDemo(wrap);
        }
      },
      initialOpts: {
        virtualize: false,
        wrap: false,
      },
      noPadding: true,
    }),
  ];
}

function renderSimpleGridDemo(wrap: boolean) {
  return m(Grid, {
    key: 'grid-demo-no-virt',
    columns: [
      {key: 'id', header: m(GridHeaderCell, 'ID')},
      {key: 'lang', header: m(GridHeaderCell, 'Language')},
      {key: 'year', header: m(GridHeaderCell, 'Year')},
      {key: 'creator', header: m(GridHeaderCell, 'Creator')},
      {key: 'typing', header: m(GridHeaderCell, 'Typing')},
    ],
    rowData: languages.map((row) => [
      m(GridCell, {wrap, align: 'right'}, row.id),
      m(GridCell, {wrap}, row.lang),
      m(GridCell, {wrap, align: 'right'}, row.year),
      m(GridCell, {wrap}, row.creator),
      m(GridCell, {wrap}, row.typing),
    ]),
    fillHeight: true,
  });
}

function VirtualGridDemo() {
  const totalRows = 10_000;
  let currentOffset = 0;
  let loadedRows: GridRow[] = [];

  const loadData = (offset: number, limit: number) => {
    currentOffset = offset;
    loadedRows = [];
    for (let i = 0; i < limit && offset + i < totalRows; i++) {
      const idx = offset + i;
      const langData = languages[idx % languages.length];
      loadedRows.push([
        m(GridCell, {align: 'right'}, idx + 1),
        m(GridCell, langData.lang),
        m(GridCell, {align: 'right'}, langData.year),
        m(GridCell, langData.creator),
        m(GridCell, langData.typing),
      ]);
    }
    m.redraw();
  };

  return {
    view: () => {
      return m(Grid, {
        key: 'virtual-grid',
        columns: [
          {key: 'id', header: m(GridHeaderCell, 'ID')},
          {key: 'lang', header: m(GridHeaderCell, 'Language')},
          {key: 'year', header: m(GridHeaderCell, 'Year')},
          {key: 'creator', header: m(GridHeaderCell, 'Creator')},
          {key: 'typing', header: m(GridHeaderCell, 'Typing')},
        ],
        rowData: {
          data: loadedRows,
          total: totalRows,
          offset: currentOffset,
          onLoadData: loadData,
        },
        virtualization: {
          rowHeightPx: 24,
        },
        fillHeight: true,
      });
    },
  };
}
