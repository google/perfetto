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
import {Box} from '../../../widgets/box';
import {Chip} from '../../../widgets/chip';
import {Stack, StackAuto} from '../../../widgets/stack';
import {ColumnDefinition, DataGridFilter} from './common';
import {DataGridApi} from './data_grid';
import {DataGridExportButton} from './export_button';

export class GridFilterBar implements m.ClassComponent {
  view({children}: m.Vnode) {
    return m(Stack, {orientation: 'horizontal', wrap: true}, children);
  }
}

export interface GridFilterAttrs {
  readonly content: string;
  onRemove(): void;
}

export class GridFilterChip implements m.ClassComponent<GridFilterAttrs> {
  view({attrs}: m.Vnode<GridFilterAttrs>): m.Children {
    return m(Chip, {
      className: 'pf-grid-filter',
      label: attrs.content,
      removable: true,
      onRemove: attrs.onRemove,
      title: attrs.content,
      removeButtonTitle: 'Remove filter',
    });
  }
}

export type OnFilterRemove = (index: number) => void;

export interface DataGridToolbarAttrs {
  readonly filters: ReadonlyArray<DataGridFilter>;
  readonly columns: ReadonlyArray<ColumnDefinition>;
  readonly totalRows: number;
  readonly showFilters: boolean;
  readonly showRowCount: boolean;
  readonly showExportButton: boolean;
  readonly toolbarItemsLeft?: m.Children;
  readonly toolbarItemsRight?: m.Children;
  readonly dataGridApi: DataGridApi;
  readonly onFilterRemove: OnFilterRemove;
  readonly formatFilter: (
    filter: DataGridFilter,
    columns: ReadonlyArray<ColumnDefinition>,
  ) => string;
}

export class DataGridToolbar implements m.ClassComponent<DataGridToolbarAttrs> {
  view({attrs}: m.Vnode<DataGridToolbarAttrs>): m.Children {
    const {
      filters,
      columns,
      totalRows,
      showFilters,
      showRowCount,
      showExportButton,
      toolbarItemsLeft,
      toolbarItemsRight,
      dataGridApi,
      onFilterRemove,
      formatFilter,
    } = attrs;

    // Build left-side toolbar items
    const leftItems: m.Children[] = [];

    if (Boolean(toolbarItemsLeft)) {
      leftItems.push(toolbarItemsLeft);
    }

    // Filter chips in auto-expanding section
    if (showFilters && filters.length > 0) {
      leftItems.push(
        m(StackAuto, [
          m(GridFilterBar, [
            filters.map((filter) => {
              return m(GridFilterChip, {
                content: formatFilter(filter, columns),
                onRemove: () => {
                  const filterIndex = filters.indexOf(filter);
                  onFilterRemove(filterIndex);
                },
              });
            }),
          ]),
        ]),
      );
    }

    // Build right-side toolbar items
    const rightItems: m.Children[] = [];

    if (showRowCount) {
      const rowCountText = `${totalRows.toLocaleString()} rows`;
      rightItems.push(m('.pf-data-grid__row-count', rowCountText));
    }

    if (Boolean(toolbarItemsRight)) {
      rightItems.push(toolbarItemsRight);
    }

    if (showExportButton) {
      rightItems.push(m(DataGridExportButton, {api: dataGridApi}));
    }

    // Only render toolbar if there are items to show
    if (leftItems.length === 0 && rightItems.length === 0) {
      return undefined;
    }

    return m(
      Box,
      {className: 'pf-data-grid__toolbar', spacing: 'small'},
      m(
        '.pf-data-grid__toolbar-content',
        m(
          Stack,
          {
            className: 'pf-data-grid__toolbar-left',
            orientation: 'horizontal',
            spacing: 'small',
          },
          leftItems,
        ),
        m(
          Stack,
          {
            className: 'pf-data-grid__toolbar-right',
            orientation: 'horizontal',
            spacing: 'small',
          },
          rightItems,
        ),
      ),
    );
  }
}
