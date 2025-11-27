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
import {Anchor} from '../../../widgets/anchor';
import {Button, ButtonVariant} from '../../../widgets/button';
import {EmptyState} from '../../../widgets/empty_state';

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
        showExportButtons: false,
      },
      noPadding: true,
    }),

    renderDocSection('DataGrid + SqlDataSource', [
      m(
        'p',
        'A DataGrid example using a data source that fetches data dynamically from trace processor.',
      ),
    ]),

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
          return m(
            EmptyState,
            {
              style: {
                height: '100%',
              },
              icon: 'search_off',
            },
            'Load a trace to start',
          );
        }
      },
      initialOpts: {
        showFiltersInToolbar: true,
        readonlyFilters: false,
        readonlySorting: false,
        aggregation: false,
      },
      noPadding: true,
    }),
  ];
}
