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
import {MenuDivider, MenuItem} from '../../../widgets/menu';
import {ColumnDefinition} from './common';

type ColumnOrder = ReadonlyArray<string>;
type OnColumnOrderChanged = (columnOrder: ColumnOrder) => void;

export function renderColumnManagementMenu(
  columns: ReadonlyArray<ColumnDefinition>,
  columnOrder: ColumnOrder,
  onColumnOrderChanged: OnColumnOrderChanged,
): m.Children {
  const allColumnsShowing = columns.every((col) =>
    columnOrder.includes(col.name),
  );

  // Show/hide columns submenu
  return m(
    MenuItem,
    {
      label: 'Manage columns',
      icon: 'view_column',
    },
    [
      // Show all
      m(MenuItem, {
        label: 'Show all',
        icon: allColumnsShowing ? Icons.Checkbox : Icons.BlankCheckbox,
        closePopupOnClick: false,
        onclick: () => {
          const newOrder = columns.map((c) => c.name);
          onColumnOrderChanged(newOrder);
        },
      }),
      m(MenuDivider),
      // Individual columns
      columns.map((col) => {
        const isVisible = columnOrder.includes(col.name);
        const columnLabel =
          col.title !== undefined ? String(col.title) : col.name;
        return m(MenuItem, {
          label: columnLabel,
          closePopupOnClick: false,
          icon: isVisible ? Icons.Checkbox : Icons.BlankCheckbox,
          onclick: () => {
            if (isVisible) {
              // Hide: remove from order (but keep at least 1 column)
              if (columnOrder.length > 1) {
                const newOrder = columnOrder.filter(
                  (name) => name !== col.name,
                );
                onColumnOrderChanged(newOrder);
              }
            } else {
              // Show: add to end of order
              const newOrder = [...columnOrder, col.name];
              onColumnOrderChanged(newOrder);
            }
          },
        });
      }),
    ],
  );
}
