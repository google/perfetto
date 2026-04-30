// Copyright (C) 2026 The Android Open Source Project
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
import {ColumnInfo} from './column_info';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Checkbox} from '../../../widgets/checkbox';
import {DraggableItem} from './widgets';

/**
 * Reusable column selector widget with checkboxes, optional drag reorder,
 * and optional per-row extra content.
 *
 * Used by ModifyColumnsNode, UnionNode, and JoinColumnSelector to replace
 * the duplicated column-selection-with-checkboxes pattern.
 */
export interface ColumnSelectorAttrs {
  // The columns to display with their checked state.
  columns: ColumnInfo[];
  // Called when columns change (check/uncheck, reorder).
  onColumnsChange: (columns: ColumnInfo[]) => void;
  // Optional help text shown above the list.
  helpText?: string;
  // Enable drag-and-drop reordering. Default: false.
  draggable?: boolean;
  // Optional per-row extra content (e.g., alias input, type selector).
  renderExtra?: (col: ColumnInfo, index: number) => m.Children;
  // Optional empty state when columns is empty.
  emptyState?: m.Child;
}

export class ColumnSelector implements m.ClassComponent<ColumnSelectorAttrs> {
  view({attrs}: m.CVnode<ColumnSelectorAttrs>) {
    const {
      columns,
      onColumnsChange,
      helpText,
      draggable,
      renderExtra,
      emptyState,
    } = attrs;

    // If columns is empty and emptyState is provided, show that instead.
    if (columns.length === 0 && emptyState !== undefined) {
      return emptyState;
    }

    return m(
      '.pf-modify-columns-content',
      // Select All / Deselect All buttons
      m(
        '.pf-select-deselect-all-buttons',
        m(Button, {
          label: 'Select All',
          onclick: () => {
            onColumnsChange(columns.map((col) => ({...col, checked: true})));
          },
          variant: ButtonVariant.Outlined,
          compact: true,
        }),
        m(Button, {
          label: 'Deselect All',
          onclick: () => {
            onColumnsChange(columns.map((col) => ({...col, checked: false})));
          },
          variant: ButtonVariant.Outlined,
          compact: true,
        }),
      ),
      // Column list
      m(
        '.pf-modify-columns-node',
        m(
          '.pf-column-list-container',
          helpText !== undefined && m('.pf-column-list-help', helpText),
          m(
            '.pf-column-list',
            columns.map((col, index) => {
              const checkbox = m(Checkbox, {
                checked: col.checked,
                label: col.name,
                onchange: (e) => {
                  const newColumns = [...columns];
                  newColumns[index] = {
                    ...newColumns[index],
                    checked: (e.target as HTMLInputElement).checked,
                  };
                  onColumnsChange(newColumns);
                },
              });

              const extra = renderExtra?.(col, index);

              if (draggable) {
                return m(
                  DraggableItem,
                  {
                    index,
                    onReorder: (from: number, to: number) => {
                      const newColumns = [...columns];
                      const [removed] = newColumns.splice(from, 1);
                      newColumns.splice(to, 0, removed);
                      onColumnsChange(newColumns);
                    },
                  },
                  checkbox,
                  extra,
                );
              }

              return m('.pf-column-selector-row', checkbox, extra);
            }),
          ),
        ),
      ),
    );
  }
}
