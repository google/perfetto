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
import {Icons} from '../../../base/semantic_icons';
import {SqlValue} from '../../../trace_processor/query_result';
import {MenuItem} from '../../../widgets/menu';
import {
  ColumnDefinition,
  DataGridFilter,
  FilterType,
  isNumeric,
  RowDef,
} from './common';

type OnFilterAdd = (filter: DataGridFilter) => void;

export interface BuiltinCellMenuItems {
  addFilter?: m.Children;
}

/**
 * Builds cell-level filter menu items based on the cell value.
 * Returns the "Add filter..." menu item that can be used in cell context menus.
 */
export function buildCellFilterMenuItems(
  column: ColumnDefinition,
  value: SqlValue,
  supportedFilters: ReadonlyArray<FilterType>,
  onFilterAdd: OnFilterAdd,
): m.Children {
  const cellFilterItems: m.Children[] = [];

  if (value !== null) {
    if (supportedFilters.includes('=')) {
      cellFilterItems.push(
        m(MenuItem, {
          label: 'Equal to this',
          onclick: () => {
            onFilterAdd({
              column: column.name,
              op: '=',
              value: value,
            });
          },
        }),
      );
    }
    if (supportedFilters.includes('!=')) {
      cellFilterItems.push(
        m(MenuItem, {
          label: 'Not equal to this',
          onclick: () => {
            onFilterAdd({
              column: column.name,
              op: '!=',
              value: value,
            });
          },
        }),
      );
    }
  }

  // Add glob filter option for string columns with text selection
  // Only show if filterType is not 'numeric'
  if (
    typeof value === 'string' &&
    supportedFilters.includes('glob') &&
    column.filterType !== 'numeric'
  ) {
    const selectedText = window.getSelection()?.toString().trim();
    if (selectedText && selectedText.length > 0) {
      cellFilterItems.push(
        m(
          MenuItem,
          {
            label: 'Filter glob',
          },
          m(MenuItem, {
            label: `"${selectedText}*"`,
            onclick: () => {
              onFilterAdd({
                column: column.name,
                op: 'glob',
                value: `${selectedText}*`,
              });
            },
          }),
          m(MenuItem, {
            label: `"*${selectedText}"`,
            onclick: () => {
              onFilterAdd({
                column: column.name,
                op: 'glob',
                value: `*${selectedText}`,
              });
            },
          }),
          m(MenuItem, {
            label: `"*${selectedText}*"`,
            onclick: () => {
              onFilterAdd({
                column: column.name,
                op: 'glob',
                value: `*${selectedText}*`,
              });
            },
          }),
        ),
      );
    }
  }

  // Numeric comparison filters - only show if filterType is not 'string'
  if (isNumeric(value) && column.filterType !== 'string') {
    if (supportedFilters.includes('>')) {
      cellFilterItems.push(
        m(MenuItem, {
          label: 'Greater than this',
          onclick: () => {
            onFilterAdd({
              column: column.name,
              op: '>',
              value: value,
            });
          },
        }),
      );
    }
    if (supportedFilters.includes('>=')) {
      cellFilterItems.push(
        m(MenuItem, {
          label: 'Greater than or equal to this',
          onclick: () => {
            onFilterAdd({
              column: column.name,
              op: '>=',
              value: value,
            });
          },
        }),
      );
    }
    if (supportedFilters.includes('<')) {
      cellFilterItems.push(
        m(MenuItem, {
          label: 'Less than this',
          onclick: () => {
            onFilterAdd({
              column: column.name,
              op: '<',
              value: value,
            });
          },
        }),
      );
    }
    if (supportedFilters.includes('<=')) {
      cellFilterItems.push(
        m(MenuItem, {
          label: 'Less than or equal to this',
          onclick: () => {
            onFilterAdd({
              column: column.name,
              op: '<=',
              value: value,
            });
          },
        }),
      );
    }
  }

  if (value === null) {
    if (supportedFilters.includes('is not null')) {
      cellFilterItems.push(
        m(MenuItem, {
          label: 'Filter out nulls',
          onclick: () => {
            onFilterAdd({
              column: column.name,
              op: 'is not null',
            });
          },
        }),
      );
    }
    if (supportedFilters.includes('is null')) {
      cellFilterItems.push(
        m(MenuItem, {
          label: 'Only show nulls',
          onclick: () => {
            onFilterAdd({
              column: column.name,
              op: 'is null',
            });
          },
        }),
      );
    }
  }

  // Return the "Add filter..." menu item if there are any filter options
  if (cellFilterItems.length > 0) {
    return m(
      MenuItem,
      {label: 'Add filter...', icon: Icons.Filter},
      cellFilterItems,
    );
  }

  return undefined;
}

/**
 * Builds the complete cell context menu items, including custom renderer support.
 */
export function buildCellContextMenu(
  column: ColumnDefinition,
  value: SqlValue,
  row: RowDef,
  supportedFilters: ReadonlyArray<FilterType>,
  onFilterAdd: OnFilterAdd,
  filterControls: boolean,
): m.Children[] {
  const menuItems: m.Children[] = [];

  if (filterControls) {
    const addFilterItem = buildCellFilterMenuItems(
      column,
      value,
      supportedFilters,
      onFilterAdd,
    );

    // Use custom cell context menu renderer if provided
    if (column.cellContextMenuRenderer) {
      const customMenuItems = column.cellContextMenuRenderer(value, row, {
        addFilter: addFilterItem,
      });
      if (customMenuItems !== undefined && customMenuItems !== null) {
        menuItems.push(customMenuItems);
      }
    } else if (addFilterItem !== undefined) {
      // Use default: just add the filter menu
      menuItems.push(addFilterItem);
    }
  }

  return menuItems;
}
