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
import {
  DataGrid,
  DataGridAttrs,
} from '../../../components/widgets/data_grid/data_grid';
import {SQLDataSource} from '../../../components/widgets/data_grid/sql_data_source';
import {Engine} from '../../../trace_processor/engine';
import {renderDocSection, renderWidgetShowcase} from '../widgets_page_utils';
import {App} from '../../../public/app';
import {languages} from '../sample_data';
import {MenuItem} from '../../../widgets/menu';
import {CodeSnippet} from '../../../widgets/code_snippet';
import {Anchor} from '../../../widgets/anchor';
import {Button, ButtonVariant} from '../../../widgets/button';

type QueryDataGridAttrs = Omit<DataGridAttrs, 'data'> & {
  readonly query: string;
  readonly engine: Engine;
};

function QueryDataGrid(vnode: m.Vnode<QueryDataGridAttrs>) {
  const dataSource = new SQLDataSource(vnode.attrs.engine, vnode.attrs.query);

  return {
    view({attrs}: m.Vnode<QueryDataGridAttrs>) {
      return m(DataGrid, {...attrs, data: dataSource});
    },
  };
}

export function renderDataGrid(app: App): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'DataGrid'),
      m('p', [
        'DataGrid is an opinionated data table and analysis tool designed for exploring ',
        'and analyzing SQL-like data with built-in sorting, filtering, and aggregation features. It is based on ',
        m(Anchor, {href: '#!/widgets/grid'}, 'Grid'),
        ' but unlike the grid component is specifically opinionated about the types of data it can receive',
      ]),
    ),

    renderWidgetShowcase({
      renderWidget: ({
        readonlyFilters,
        readonlySorting,
        aggregation,
        demoToolbarItems,
        ...rest
      }) =>
        m(DataGrid, {
          ...rest,
          toolbarItemsLeft: demoToolbarItems
            ? m(Button, {
                label: 'Left Action',
                variant: ButtonVariant.Filled,
              })
            : undefined,
          toolbarItemsRight: demoToolbarItems
            ? m(Button, {
                label: 'Right Action',
                variant: ButtonVariant.Filled,
              })
            : undefined,
          fillHeight: true,
          filters: readonlyFilters ? [] : undefined,
          sorting: readonlySorting ? {direction: 'UNSORTED'} : undefined,
          columns: [
            {
              name: 'id',
              title: 'ID',
              aggregation: aggregation ? 'COUNT' : undefined,
              headerMenuItems: m(MenuItem, {
                label: 'Log column name',
                icon: 'info',
                onclick: () => console.log('Column: id'),
              }),
            },
            {
              name: 'lang',
              title: 'Language',
            },
            {
              name: 'year',
              title: 'Year',
            },
            {
              name: 'creator',
              title: 'Creator',
            },
            {
              name: 'typing',
              title: 'Typing',
            },
          ],
          data: languages,
        }),
      initialOpts: {
        showFiltersInToolbar: true,
        readonlyFilters: false,
        readonlySorting: false,
        aggregation: false,
        showResetButton: false,
        demoToolbarItems: false,
      },
    }),

    renderDocSection('Key Features', [
      m('ul', [
        m('li', [
          m('strong', 'Data Sources: '),
          'Supports in-memory (JavaScript arrays), SQL-backed (trace processor queries), ',
          'and custom data sources',
        ]),
        m('li', [
          m('strong', 'Sorting: '),
          'Sort by any column (ascending, descending, or unsorted) in controlled or uncontrolled mode',
        ]),
        m('li', [
          m('strong', 'Filtering: '),
          'Filter rows using operators: =, !=, <, >, <=, >=, glob, is null, is not null',
        ]),
        m('li', [
          m('strong', 'Aggregations: '),
          'Display aggregate values (SUM, AVG, COUNT, MIN, MAX) in column headers',
        ]),
        m('li', [
          m('strong', 'Column Management: '),
          'Drag-and-drop reordering, show/hide columns',
        ]),
        m('li', [
          m('strong', 'Virtualization: '),
          'Efficient rendering of large datasets with virtual scrolling',
        ]),
        m('li', [
          m('strong', 'Customization: '),
          'Custom cell renderers, header menu items, and cell menu items',
        ]),
      ]),
    ]),
    renderDocSection('End User Interaction Guide', [
      m('p', [
        'DataGrid provides rich interactive features for exploring data. ',
        'Users can sort, filter, and manage columns through mouse and keyboard interactions.',
      ]),
      m('h3', 'Sorting'),
      m('ul', [
        m('li', [
          m('strong', 'Click on column headers'),
          ' to sort by that column. First click sorts ascending, second click sorts descending, third click removes sorting.',
        ]),
      ]),
      m('h3', 'Filtering'),
      m('ul', [
        m('li', [
          m('strong', 'Right-click on any cell'),
          ' to open a context menu with filter options.',
        ]),
        m('li', [
          'Select a filter operator (=, !=, <, >, <=, >=, glob, is null, is not null) to add a filter for that column and value.',
        ]),
        m('li', [
          'Applied filters appear as chips in the toolbar (if ',
          m('code', 'showFiltersInToolbar: true'),
          ').',
        ]),
        m('li', ['Click the × on a filter chip to remove that filter.']),
      ]),
      m('h3', 'Column Management'),
      m('ul', [
        m('li', [
          m('strong', 'Click the triple dot menu on column headers'),
          ' to access column-specific actions and custom menu items.',
        ]),
        m('li', [
          m('strong', 'Drag column headers'),
          ' to reorder columns (when column reordering is enabled).',
        ]),
        m('li', ['Use header menu to show/hide columns (when available).']),
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
      m('h3', 'Toolbar Actions'),
      m('ul', [
        m('li', [
          'When ',
          m('code', 'showResetButton: true'),
          ', a reset button appears in the toolbar to clear all filters and sorting.',
        ]),
        m('li', [
          'Custom toolbar items may provide additional actions like export, refresh, etc.',
        ]),
      ]),
    ]),

    renderDocSection('Basic Usage', [
      m('p', 'Simplest in-memory data source:'),
      m(CodeSnippet, {
        text: `m(DataGrid, {
  columns: [
    {name: 'id', title: 'ID'},
    {name: 'name', title: 'Name'},
    {name: 'value', title: 'Value'},
  ],
  data: [
    {id: 1, name: 'Item 1', value: 100},
    {id: 2, name: 'Item 2', value: 200},
  ],
  fillHeight: true,
})`,
        language: 'typescript',
      }),
    ]),

    renderDocSection('SQL Data Source', [
      m('p', 'For SQL-backed data, use SQLDataSource:'),
      m(CodeSnippet, {
        text: `const dataSource = new SQLDataSource(
  engine,
  'SELECT * FROM slice LIMIT 100'
);

m(DataGrid, {
  columns: [
    {name: 'id', title: 'Slice ID'},
    {name: 'ts', title: 'Timestamp'},
    {name: 'dur', title: 'Duration'},
  ],
  data: dataSource,
  fillHeight: true,
})`,
        language: 'typescript',
      }),
    ]),

    renderDocSection('Aggregations', [
      m('p', 'Add aggregate functions to display summary values:'),
      m(CodeSnippet, {
        text: `columns: [
  {name: 'id', title: 'ID', aggregation: 'COUNT'},
  {name: 'dur', title: 'Duration', aggregation: 'SUM'},
  {name: 'value', title: 'Value', aggregation: 'AVG'},
]`,
        language: 'typescript',
      }),
      m('p', [
        m('strong', 'Available functions: '),
        "'SUM', 'AVG', 'COUNT', 'MIN', 'MAX'",
      ]),
    ]),

    renderDocSection('Controlled State', [
      m(
        'p',
        'Full control over grid state for persistence or synchronization:',
      ),
      m(CodeSnippet, {
        text: `let sorting = {direction: 'UNSORTED'};
let filters = [];

m(DataGrid, {
  columns: [...],
  data: dataSource,
  sorting: sorting,
  onSort: (newSorting) => { sorting = newSorting; },
  filters: filters,
  onFilterAdd: (filter) => { filters = [...filters, filter]; },
  onFilterRemove: (index) => {
    filters = filters.filter((_, i) => i !== index);
  },
})`,
        language: 'typescript',
      }),
    ]),

    renderDocSection('Custom Cell Rendering', [
      m('p', 'Customize how cell values are displayed:'),
      m(CodeSnippet, {
        text: `cellRenderer: (value, columnName, row) => {
  if (columnName === 'id' && row.slice_id !== undefined) {
    return m(Anchor, {
      title: 'Go to slice',
      icon: Icons.UpdateSelection,
      onclick: () => goToSlice(row.slice_id),
    }, String(value));
  }
  return String(value);
}`,
        language: 'typescript',
      }),
    ]),

    renderDocSection('Custom Menu Items', [
      m('p', 'Add custom actions to header or cell context menus:'),
      m(CodeSnippet, {
        text: `columns: [
  {
    name: 'id',
    title: 'ID',
    headerMenuItems: m(MenuItem, {
      label: 'Log column name',
      icon: 'info',
      onclick: () => console.log('Column: id'),
    }),
    cellMenuItems: (value, row) => m(MenuItem, {
      label: \`Process \${value}\`,
      onclick: () => processRow(row),
    }),
  },
]`,
        language: 'typescript',
      }),
    ]),

    renderDocSection('Important Tips', [
      m('ol', [
        m('li', [
          m('strong', 'Container Height (Critical for Virtualization): '),
          'DataGrid requires a fixed height to enable virtual scrolling. ',
          m('strong', 'The grid itself must scroll, not an external container'),
          ', ',
          'otherwise virtualization will not work properly. ',
          'Always wrap in a container with explicit height (e.g., ',
          m('code', "style: {height: '400px'}"),
          '). If the parent container is resizable (like a panel or split view), use ',
          m('code', 'fillHeight: true'),
          ' to make the grid fill its parent and handle scrolling internally.',
        ]),
        m('li', [
          m('strong', 'Read-Only Filtering: '),
          'Pass an empty filters array to show filters without allowing modifications: ',
          m('code', 'filters: []'),
        ]),
        m('li', [
          m('strong', 'Column Reordering: '),
          'Enabled by default when onColumnOrderChanged is provided. Disable with ',
          m('code', 'columnReordering: false'),
        ]),
        m('li', [
          m('strong', 'Initial State: '),
          'Use initialSorting, initialFilters, and initialColumnOrder for uncontrolled mode defaults',
        ]),
        m('li', [
          m('strong', 'Toolbar Customization: '),
          'Add custom buttons with toolbarItemsLeft/Right. Enable reset button with ',
          m('code', 'showResetButton: true'),
        ]),
        m('li', [
          m('strong', 'Performance: '),
          'For large datasets, prefer SQLDataSource over InMemoryDataSource to push filtering/sorting to the database',
        ]),
      ]),
    ]),

    m('h2', 'Live Examples'),

    renderWidgetShowcase({
      renderWidget: ({
        readonlyFilters,
        readonlySorting,
        aggregation,
        ...rest
      }) => {
        const trace = app.trace;
        if (trace) {
          return m(QueryDataGrid, {
            ...rest,
            engine: trace.engine,
            query: `
              SELECT
                ts.id as id,
                dur,
                state,
                thread.name as thread_name,
                dur,
                io_wait,
                ucpu
              FROM thread_state ts
              JOIN thread USING(utid)
            `,
            fillHeight: true,
            filters: readonlyFilters ? [] : undefined,
            sorting: readonlySorting ? {direction: 'UNSORTED'} : undefined,
            columns: [
              {
                name: 'id',
                title: 'ID',
                aggregation: aggregation ? 'COUNT' : undefined,
              },
              {
                name: 'dur',
                title: 'Duration',
                aggregation: aggregation ? 'SUM' : undefined,
              },
              {name: 'state', title: 'State'},
              {name: 'thread_name', title: 'Thread'},
              {name: 'ucpu', title: 'CPU'},
              {name: 'io_wait', title: 'IO Wait'},
            ],
          });
        } else {
          return 'Load a trace to start';
        }
      },
      initialOpts: {
        showFiltersInToolbar: true,
        readonlyFilters: false,
        readonlySorting: false,
        aggregation: false,
      },
    }),
  ];
}
