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
import {DataGrid} from '../../components/widgets/data_grid/data_grid';
import {MenuItem} from '../../widgets/menu';
import {renderWidgetShowcase} from './widget_page_utils';
import {languages} from './sample_data';

export function renderDataGridDemo() {
  return renderWidgetShowcase({
    label: 'DataGrid (memory backed)',
    description: `An interactive data explorer and viewer.`,
    renderWidget: ({readonlyFilters, readonlySorting, aggregation, ...rest}) =>
      m(
        '',
        {style: {height: '400px', width: '800px', overflow: 'hidden'}},
        m(DataGrid, {
          ...rest,
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
      ),
    initialOpts: {
      showFiltersInToolbar: true,
      readonlyFilters: false,
      readonlySorting: false,
      aggregation: false,
    },
  });
}
