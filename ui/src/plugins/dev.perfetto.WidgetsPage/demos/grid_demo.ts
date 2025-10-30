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
import {renderDocSection, renderWidgetShowcase} from '../widgets_page_utils';
import {languages} from '../sample_data';
import {CodeSnippet} from '../../../widgets/code_snippet';

export function renderGrid(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Grid'),
      m('p', [
        'Grid is a purely presentational component for rendering tabular data with ',
        'virtual scrolling and column resizing. Unlike DataGrid, it provides no automatic ',
        'features like sorting or filtering - you must provide all content as GridCell and ',
        'GridHeaderCell components.',
      ]),
    ),

    renderWidgetShowcase({
      renderWidget: ({virtualize}) => {
        if (virtualize) {
          return m(VirtualGridDemo);
        }
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
            m(GridCell, {align: 'right'}, row.id),
            m(GridCell, row.lang),
            m(GridCell, {align: 'right'}, row.year),
            m(GridCell, row.creator),
            m(GridCell, row.typing),
          ]),
          fillHeight: true,
        });
      },
      initialOpts: {virtualize: false},
    }),

    renderDocSection('End User Interaction Guide', [
      m('p', [
        'Grid provides basic interactive features for viewing and navigating data. ',
        'As a presentational component, advanced features like sorting and filtering ',
        'must be implemented by the parent component.',
      ]),
      m('h3', 'Column Resizing'),
      m('ul', [
        m('li', [
          m('strong', 'Drag the resize handle'),
          ' between column headers to manually adjust column width.',
        ]),
        m('li', [
          m('strong', 'Double-click the resize handle'),
          ' to auto-resize the column to fit its content.',
        ]),
      ]),
      m('h3', 'Column Reordering'),
      m('ul', [
        m('li', [
          m('strong', 'Drag column headers'),
          ' to reorder columns (when column reordering is enabled and configured).',
        ]),
        m('li', [
          'Drop indicators show where the column will be placed when released.',
        ]),
      ]),
      m('h3', 'Context Menus'),
      m('ul', [
        m('li', [
          m('strong', 'Right-click on cells'),
          ' to access custom cell menu items (when configured by the parent component).',
        ]),
        m('li', [
          m('strong', 'Click the menu button'),
          ' in column headers to access custom header menu items (when configured).',
        ]),
      ]),
      m('h3', 'Navigation'),
      m('ul', [
        m('li', [
          m('strong', 'Scroll'),
          ' vertically to navigate through rows. The grid uses virtual scrolling for efficient rendering of large datasets.',
        ]),
        m('li', [
          m('strong', 'Scroll'),
          " horizontally to view columns that don't fit in the viewport.",
        ]),
      ]),
      m('h3', 'Sorting (When Configured)'),
      m('ul', [
        m('li', [
          'When the parent component configures sorting on a ',
          m('strong', 'GridHeaderCell'),
          ', click the sort button to cycle through ascending, descending, and unsorted states.',
        ]),
      ]),
    ]),

    renderDocSection('Key Features', [
      m('ul', [
        m('li', [
          m('strong', 'Presentational Component: '),
          'Grid is purely presentational - you provide all cells as GridCell and GridHeaderCell components',
        ]),
        m('li', [
          m('strong', 'Virtual Scrolling: '),
          'Efficient rendering of large datasets with DOM virtualization',
        ]),
        m('li', [
          m('strong', 'Partial Data Loading: '),
          'Support for paginated/lazy loading of data with PartialRowData',
        ]),
        m('li', [
          m('strong', 'Column Resizing: '),
          'Manual drag-to-resize and auto-sizing based on content',
        ]),
        m('li', [
          m('strong', 'Column Reordering: '),
          'Drag-and-drop column reordering when configured',
        ]),
        m('li', [
          m('strong', 'Automatic Column Sizing: '),
          'Measures content and sets optimal initial widths',
        ]),
      ]),
    ]),

    renderDocSection('Basic Usage', [
      m('p', 'Simple grid with all data in memory (no virtualization):'),
      m(CodeSnippet, {
        text: `m(Grid, {
  columns: [
    {key: 'id', header: m(GridHeaderCell, 'ID')},
    {key: 'name', header: m(GridHeaderCell, 'Name')},
  ],
  rowData: [
    [m(GridCell, {align: 'right'}, '1'), m(GridCell, 'Alice')],
    [m(GridCell, {align: 'right'}, '2'), m(GridCell, 'Bob')],
  ],
  fillHeight: true,
})`,
        language: 'typescript',
      }),
    ]),

    renderDocSection('Virtualization', [
      m('p', 'Enable DOM virtualization for large datasets:'),
      m(CodeSnippet, {
        text: `m(Grid, {
  columns: [...],
  rowData: [...], // Full dataset
  virtualization: {
    rowHeightPx: 24,  // Fixed row height for virtual scrolling
  },
  fillHeight: true,
})`,
        language: 'typescript',
      }),
    ]),

    renderDocSection('Partial Data Loading', [
      m(
        'p',
        'For very large datasets, load data on-demand as the user scrolls:',
      ),
      m(CodeSnippet, {
        text: `let currentOffset = 0;
let loadedRows: GridRow[] = [];
const totalRows = 1000000;

const loadData = (offset: number, limit: number) => {
  currentOffset = offset;
  loadedRows = fetchRowsFromAPI(offset, limit);
  m.redraw();
};

m(Grid, {
  columns: [...],
  rowData: {
    data: loadedRows,      // Currently loaded rows
    total: totalRows,      // Total number of rows
    offset: currentOffset, // Current offset
    onLoadData: loadData,  // Callback to load more data
  },
  virtualization: {
    rowHeightPx: 24,  // Required for PartialRowData
  },
  fillHeight: true,
})`,
        language: 'typescript',
      }),
    ]),

    renderDocSection('Column Configuration', [
      m('p', 'Columns support various configuration options:'),
      m(CodeSnippet, {
        text: `columns: [
  {
    key: 'id',
    header: m(GridHeaderCell, 'ID'),
    minWidth: 80,              // Minimum width in pixels
    maxInitialWidthPx: 200,    // Maximum width during auto-sizing
    thickRightBorder: true,    // Visual separator
  },
  {
    key: 'actions',
    header: m(GridHeaderCell, 'Actions'),
    reorderable: {             // Enable drag-and-drop reordering
      handle: 'my-drag-handle',
    },
  },
]`,
        language: 'typescript',
      }),
    ]),

    renderDocSection('Sorting', [
      m('p', 'Add sorting to column headers:'),
      m(CodeSnippet, {
        text: `let sortColumn: string | undefined;
let sortDirection: SortDirection | undefined;

const handleSort = (column: string, direction: SortDirection) => {
  sortColumn = column;
  sortDirection = direction;
  // Sort your data and redraw
};

columns: [
  {
    key: 'name',
    header: m(GridHeaderCell, {
      sort: sortColumn === 'name' ? sortDirection : undefined,
      onSort: (dir) => handleSort('name', dir),
    }, 'Name'),
  },
]`,
        language: 'typescript',
      }),
    ]),

    renderDocSection('Custom Menu Items', [
      m('p', 'Add context menus to headers and cells:'),
      m(CodeSnippet, {
        text: `columns: [
  {
    key: 'id',
    header: m(GridHeaderCell, {
      menuItems: [
        m(MenuItem, {
          label: 'Hide column',
          icon: 'visibility_off',
          onclick: () => hideColumn('id'),
        }),
      ],
    }, 'ID'),
  },
]

// In cell rendering
rowData: data.map(row => [
  m(GridCell, {
    menuItems: [
      m(MenuItem, {
        label: 'Copy ID',
        onclick: () => copyToClipboard(row.id),
      }),
    ],
  }, row.id),
])`,
        language: 'typescript',
      }),
    ]),

    renderDocSection('Column Reordering', [
      m('p', 'Enable drag-and-drop column reordering:'),
      m(CodeSnippet, {
        text: `let columnOrder = ['id', 'name', 'value'];

const handleReorder = (from: string, to: string, position: ReorderPosition) => {
  // Reorder your columns array
  const fromIndex = columnOrder.indexOf(from);
  const toIndex = columnOrder.indexOf(to);
  
  const newOrder = [...columnOrder];
  newOrder.splice(fromIndex, 1);
  const insertIndex = position === 'before' ? toIndex : toIndex + 1;
  newOrder.splice(insertIndex, 0, from);
  
  columnOrder = newOrder;
};

m(Grid, {
  columns: columnOrder.map(key => ({
    key,
    header: m(GridHeaderCell, titles[key]),
    reorderable: {handle: 'column-reorder'},
  })),
  rowData: [...],
  onColumnReorder: handleReorder,
})`,
        language: 'typescript',
      }),
    ]),

    renderDocSection('Important Tips', [
      m('ol', [
        m('li', [
          m('strong', 'Container Height (Critical): '),
          'Grid requires a fixed height to enable virtual scrolling. Always wrap in a container with explicit height or use ',
          m('code', 'fillHeight: true'),
          ' to fill the parent container.',
        ]),
        m('li', [
          m('strong', 'Cell Wrapping: '),
          'Grid does NO automatic wrapping. You must wrap all cell content in ',
          m('code', 'GridCell'),
          ' components and all headers in ',
          m('code', 'GridHeaderCell'),
          ' components.',
        ]),
        m('li', [
          m('strong', 'Fixed Row Heights: '),
          'When using virtualization, all rows must have the same height specified in ',
          m('code', 'virtualization.rowHeightPx'),
          '.',
        ]),
        m('li', [
          m('strong', 'PartialRowData Requires Virtualization: '),
          'If using PartialRowData for lazy loading, virtualization is mandatory.',
        ]),
        m('li', [
          m('strong', 'For Automatic Features, Use DataGrid: '),
          'Grid is a low-level presentational component. If you need sorting, filtering, or aggregations, use DataGrid instead.',
        ]),
        m('li', [
          m('strong', 'Column Auto-Sizing: '),
          'Grid automatically measures and sizes columns on first render. Double-click resize handles to re-measure a column.',
        ]),
      ]),
    ]),
  ];
}

export function VirtualGridDemo() {
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
