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
import {SqlValue} from '../../../trace_processor/query_result';
import {MenuItem} from '../../../widgets/menu';
import {OnFilterAdd} from './column_filter_menu';
import {Icons} from '../../../base/semantic_icons';
import {isNumeric} from '../../../base/utils';

export function renderCellFilterMenuItem({
  columnPath,
  value,
  onFilterAdd,
  colFilterType,
}: {
  readonly columnPath: string;
  readonly value: SqlValue;
  readonly onFilterAdd: OnFilterAdd;
  readonly colFilterType: undefined | 'numeric' | 'string';
}): m.Children {
  const cellFilterItems = [];

  if (value !== null) {
    cellFilterItems.push(
      m(MenuItem, {
        label: 'Equal to this',
        onclick: () => {
          onFilterAdd({
            column: columnPath,
            op: '=',
            value: value,
          });
        },
      }),
    );
    cellFilterItems.push(
      m(MenuItem, {
        label: 'Not equal to this',
        onclick: () => {
          onFilterAdd({
            column: columnPath,
            op: '!=',
            value: value,
          });
        },
      }),
    );
  }

  // Add glob filter option for string columns with text selection
  // Only show if filterType is not 'numeric'
  if (typeof value === 'string' && colFilterType !== 'numeric') {
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
                column: columnPath,
                op: 'glob',
                value: `${selectedText}*`,
              });
            },
          }),
          m(MenuItem, {
            label: `"*${selectedText}"`,
            onclick: () => {
              onFilterAdd({
                column: columnPath,
                op: 'glob',
                value: `*${selectedText}`,
              });
            },
          }),
          m(MenuItem, {
            label: `"*${selectedText}*"`,
            onclick: () => {
              onFilterAdd({
                column: columnPath,
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
  if (isNumeric(value) && colFilterType !== 'string') {
    cellFilterItems.push(
      m(MenuItem, {
        label: 'Greater than this',
        onclick: () => {
          onFilterAdd({
            column: columnPath,
            op: '>',
            value: value,
          });
        },
      }),
    );
    cellFilterItems.push(
      m(MenuItem, {
        label: 'Greater than or equal to this',
        onclick: () => {
          onFilterAdd({
            column: columnPath,
            op: '>=',
            value: value,
          });
        },
      }),
    );
    cellFilterItems.push(
      m(MenuItem, {
        label: 'Less than this',
        onclick: () => {
          onFilterAdd({
            column: columnPath,
            op: '<',
            value: value,
          });
        },
      }),
    );
    cellFilterItems.push(
      m(MenuItem, {
        label: 'Less than or equal to this',
        onclick: () => {
          onFilterAdd({
            column: columnPath,
            op: '<=',
            value: value,
          });
        },
      }),
    );
  }

  if (value === null) {
    cellFilterItems.push(
      m(MenuItem, {
        label: 'Filter out nulls',
        onclick: () => {
          onFilterAdd({
            column: columnPath,
            op: 'is not null',
          });
        },
      }),
    );
    cellFilterItems.push(
      m(MenuItem, {
        label: 'Only show nulls',
        onclick: () => {
          onFilterAdd({
            column: columnPath,
            op: 'is null',
          });
        },
      }),
    );
  }

  // Build "Add filter..." menu item to pass to renderer
  const addFilterItem =
    cellFilterItems.length > 0
      ? m(
          MenuItem,
          {label: 'Add filter...', icon: Icons.Filter},
          cellFilterItems,
        )
      : undefined;

  return addFilterItem;
}
