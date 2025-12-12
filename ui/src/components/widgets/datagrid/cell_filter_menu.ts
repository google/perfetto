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
import {isNumeric} from '../../../base/utils';
import {SqlValue} from '../../../trace_processor/query_result';
import {MenuItem} from '../../../widgets/menu';
import {FilterOpAndValue} from './model';

interface CellFilterMenuAttrs {
  readonly value: SqlValue;
  readonly onFilterAdd: (filter: FilterOpAndValue) => void;
}

/**
 * Renders "Add filter" menu item for cell context menus.
 * Shows filter options based on the cell's value type.
 */
export class CellFilterMenu implements m.ClassComponent<CellFilterMenuAttrs> {
  view({attrs}: m.Vnode<CellFilterMenuAttrs>): m.Children {
    const {value, onFilterAdd} = attrs;
    const cellFilterItems: m.Children[] = [];

    if (value !== null) {
      cellFilterItems.push(
        m(MenuItem, {
          label: 'Equals',
          onclick: () => onFilterAdd({op: '=', value}),
        }),
      );
      cellFilterItems.push(
        m(MenuItem, {
          label: 'Not equals',
          onclick: () => onFilterAdd({op: '!=', value}),
        }),
      );
    }

    // Add glob filter option for string columns with text selection
    if (typeof value === 'string') {
      const selectedText = window.getSelection()?.toString().trim();
      if (selectedText && selectedText.length > 0) {
        cellFilterItems.push(
          m(
            MenuItem,
            {label: 'Glob'},
            m(MenuItem, {
              label: `"${selectedText}*"`,
              onclick: () =>
                onFilterAdd({op: 'glob', value: `${selectedText}*`}),
            }),
            m(MenuItem, {
              label: `"*${selectedText}"`,
              onclick: () =>
                onFilterAdd({op: 'glob', value: `*${selectedText}`}),
            }),
            m(MenuItem, {
              label: `"*${selectedText}*"`,
              onclick: () =>
                onFilterAdd({op: 'glob', value: `*${selectedText}*`}),
            }),
          ),
        );
      }
    }

    // Numeric comparison filters
    if (isNumeric(value)) {
      cellFilterItems.push(
        m(MenuItem, {
          label: 'Greater than',
          onclick: () => onFilterAdd({op: '>', value}),
        }),
      );
      cellFilterItems.push(
        m(MenuItem, {
          label: 'Greater than or equals',
          onclick: () => onFilterAdd({op: '>=', value}),
        }),
      );
      cellFilterItems.push(
        m(MenuItem, {
          label: 'Less than',
          onclick: () => onFilterAdd({op: '<', value}),
        }),
      );
      cellFilterItems.push(
        m(MenuItem, {
          label: 'Less than or equals',
          onclick: () => onFilterAdd({op: '<=', value}),
        }),
      );
    }

    if (value === null) {
      cellFilterItems.push(
        m(MenuItem, {
          label: 'Is not null',
          onclick: () => onFilterAdd({op: 'is not null'}),
        }),
      );
      cellFilterItems.push(
        m(MenuItem, {
          label: 'Is null',
          onclick: () => onFilterAdd({op: 'is null'}),
        }),
      );
    }

    if (cellFilterItems.length === 0) {
      return undefined;
    }

    return m(
      MenuItem,
      {label: 'Add filter', icon: Icons.Filter},
      cellFilterItems,
    );
  }
}
