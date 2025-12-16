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
import {ColumnInfo} from './column_info';
import {
  OutlinedField,
  DataExplorerEmptyState,
  SelectDeselectAllButtons,
} from './widgets';
import {classNames} from '../../../base/classnames';
import {Card} from '../../../widgets/card';
import {Checkbox} from '../../../widgets/checkbox';
import {TextInput} from '../../../widgets/text_input';

/**
 * Widget for displaying a join source as a card
 * Shows join column selector and checkboxes for selecting other columns
 */
export interface JoinSourceCardAttrs {
  label: string; // e.g., "Left", "Right"
  columns: ColumnInfo[];
  otherSideColumns: ColumnInfo[]; // Columns from the other side to check for duplicates
  selectedColumn?: string;
  joinColumnDisabled?: boolean; // Whether the join column selector should be disabled
  onColumnChange: (columnName: string) => void;
  onColumnToggle: (index: number, checked: boolean) => void;
  onColumnAlias: (index: number, alias: string) => void;
}

export class JoinSourceCard implements m.ClassComponent<JoinSourceCardAttrs> {
  view({attrs}: m.Vnode<JoinSourceCardAttrs>) {
    const {
      label,
      columns,
      otherSideColumns,
      selectedColumn,
      joinColumnDisabled,
      onColumnChange,
      onColumnToggle,
      onColumnAlias,
    } = attrs;

    // Show empty state if no columns are available (no node connected)
    if (columns.length === 0) {
      return m(
        Card,
        {className: 'pf-join-source-card'},
        m('.pf-join-source-card__header', label),
        m(
          '.pf-join-source-card__content',
          m(DataExplorerEmptyState, {
            icon: 'cable',
            title: `Connect a node to the ${label} input`,
          }),
        ),
      );
    }

    return m(
      Card,
      {className: 'pf-join-source-card'},
      m('.pf-join-source-card__header', label),
      m(
        '.pf-join-source-card__content',
        // Join column selector
        m(
          OutlinedField,
          {
            label: 'Join Column',
            value: selectedColumn || '',
            disabled: joinColumnDisabled,
            onchange: (e: Event) => {
              const target = e.target as HTMLSelectElement;
              onColumnChange(target.value);
            },
          },
          [
            m(
              'option',
              {
                value: '',
                selected: !selectedColumn,
                disabled: true,
              },
              'Select column',
            ),
            ...columns.map((col) =>
              m(
                'option',
                {
                  value: col.column.name,
                  selected: col.column.name === selectedColumn,
                },
                col.column.name,
              ),
            ),
          ],
        ),
        // Column selection with checkboxes and aliasing
        columns.length > 0 &&
          m(
            '.pf-join-source-card__columns',
            m(
              '.pf-join-source-card__columns-label',
              'Select columns to include:',
            ),
            m(
              '.pf-join-source-card__columns-list',
              columns.map((col, index) => {
                // Check if this column's final name conflicts with the other side
                // Use alias if available, otherwise use the column name
                const finalName = col.alias ?? col.column.name;
                const isDuplicate = otherSideColumns.some(
                  (otherCol) =>
                    otherCol.checked &&
                    (otherCol.alias ?? otherCol.column.name) === finalName,
                );
                const isDisabled = isDuplicate && !col.checked;

                return m(
                  '.pf-join-column-row',
                  {
                    className: classNames(isDisabled && 'pf-disabled'),
                  },
                  m(Checkbox, {
                    checked: col.checked,
                    disabled: isDisabled,
                    label: col.column.name,
                    onchange: (e) => {
                      onColumnToggle(
                        index,
                        (e.target as HTMLInputElement).checked,
                      );
                    },
                  }),
                  m(TextInput, {
                    // Always allow editing alias - that's how users resolve conflicts
                    oninput: (e: Event) => {
                      const inputValue = (e.target as HTMLInputElement).value;
                      onColumnAlias(
                        index,
                        inputValue.trim() === '' ? '' : inputValue,
                      );
                    },
                    placeholder: 'alias',
                    value: col.alias ?? '',
                  }),
                );
              }),
            ),
          ),
      ),
    );
  }
}

/**
 * Widget for displaying the join condition between two sources
 * Shows two cards side-by-side for left and right sources
 */
export interface JoinConditionSelectorAttrs {
  leftLabel: string; // e.g., "Left", "Primary"
  rightLabel: string; // e.g., "Right", "Secondary"
  leftColumns: ColumnInfo[];
  rightColumns: ColumnInfo[];
  leftColumn?: string;
  rightColumn?: string;
  onLeftColumnChange: (columnName: string) => void;
  onRightColumnChange: (columnName: string) => void;
  onLeftColumnToggle: (index: number, checked: boolean) => void;
  onRightColumnToggle: (index: number, checked: boolean) => void;
  onLeftColumnAlias: (index: number, alias: string) => void;
  onRightColumnAlias: (index: number, alias: string) => void;
}

export class JoinConditionSelector
  implements m.ClassComponent<JoinConditionSelectorAttrs>
{
  view({attrs}: m.Vnode<JoinConditionSelectorAttrs>) {
    const {
      leftLabel,
      rightLabel,
      leftColumns,
      rightColumns,
      leftColumn,
      rightColumn,
      onLeftColumnChange,
      onRightColumnChange,
      onLeftColumnToggle,
      onRightColumnToggle,
      onLeftColumnAlias,
      onRightColumnAlias,
    } = attrs;

    const hasValidJoin = leftColumn && rightColumn;

    return m(
      '.pf-join-condition-selector',
      {
        className: classNames(hasValidJoin && 'pf-join-condition--complete'),
      },
      m(
        '.pf-join-condition-selector__cards',
        // Left source card
        m(JoinSourceCard, {
          label: leftLabel,
          columns: leftColumns,
          otherSideColumns: rightColumns,
          selectedColumn: leftColumn,
          onColumnChange: onLeftColumnChange,
          onColumnToggle: onLeftColumnToggle,
          onColumnAlias: onLeftColumnAlias,
        }),
        // Right source card
        m(JoinSourceCard, {
          label: rightLabel,
          columns: rightColumns,
          otherSideColumns: leftColumns,
          selectedColumn: rightColumn,
          onColumnChange: onRightColumnChange,
          onColumnToggle: onRightColumnToggle,
          onColumnAlias: onRightColumnAlias,
        }),
      ),
    );
  }
}

/**
 * Compact join condition display (read-only)
 * Used in node details view to show the current join condition
 */
export interface JoinConditionDisplayAttrs {
  leftAlias: string;
  rightAlias: string;
  leftColumn: string;
  rightColumn: string;
  operator?: string;
}

export class JoinConditionDisplay
  implements m.ClassComponent<JoinConditionDisplayAttrs>
{
  view({attrs}: m.Vnode<JoinConditionDisplayAttrs>) {
    const {
      leftAlias,
      rightAlias,
      leftColumn,
      rightColumn,
      operator = '=',
    } = attrs;

    return m('.pf-join-condition-display', [
      m('span.pf-join-column-ref', `${leftAlias}.${leftColumn}`),
      m('span.pf-join-operator', ` ${operator} `),
      m('span.pf-join-column-ref', `${rightAlias}.${rightColumn}`),
    ]);
  }
}

/**
 * Component for selecting which columns to include from a join
 * Shows columns from both left and right sources with checkboxes and aliasing
 */
export interface JoinColumnSelectorAttrs {
  leftAlias: string;
  rightAlias: string;
  leftColumns: ColumnInfo[];
  rightColumns: ColumnInfo[];
  onLeftColumnToggle: (index: number, checked: boolean) => void;
  onRightColumnToggle: (index: number, checked: boolean) => void;
  onLeftColumnAlias: (index: number, alias: string) => void;
  onRightColumnAlias: (index: number, alias: string) => void;
  onSelectAllLeft: () => void;
  onDeselectAllLeft: () => void;
  onSelectAllRight: () => void;
  onDeselectAllRight: () => void;
}

export class JoinColumnSelector
  implements m.ClassComponent<JoinColumnSelectorAttrs>
{
  private renderColumnRow(
    col: ColumnInfo,
    index: number,
    onToggle: (index: number, checked: boolean) => void,
    onAlias: (index: number, alias: string) => void,
  ): m.Child {
    return m(
      '.pf-join-column-row',
      m(Checkbox, {
        checked: col.checked,
        label: col.column.name,
        onchange: (e) => {
          onToggle(index, (e.target as HTMLInputElement).checked);
        },
      }),
      m(TextInput, {
        oninput: (e: Event) => {
          const inputValue = (e.target as HTMLInputElement).value;
          // Normalize empty strings to undefined (no alias)
          onAlias(index, inputValue.trim() === '' ? '' : inputValue);
        },
        placeholder: 'alias',
        value: col.alias ?? '',
      }),
    );
  }

  view({attrs}: m.Vnode<JoinColumnSelectorAttrs>) {
    const {
      leftAlias,
      rightAlias,
      leftColumns,
      rightColumns,
      onLeftColumnToggle,
      onRightColumnToggle,
      onLeftColumnAlias,
      onRightColumnAlias,
      onSelectAllLeft,
      onDeselectAllLeft,
      onSelectAllRight,
      onDeselectAllRight,
    } = attrs;

    const leftSelectedCount = leftColumns.filter((c) => c.checked).length;
    const rightSelectedCount = rightColumns.filter((c) => c.checked).length;

    return m('.pf-join-column-selector', [
      // Left columns section
      m('.pf-join-column-section', [
        m(
          '.pf-join-column-section__header',
          m(
            'h4',
            `${leftAlias} (${leftSelectedCount} / ${leftColumns.length} selected)`,
          ),
          m(SelectDeselectAllButtons, {
            onSelectAll: onSelectAllLeft,
            onDeselectAll: onDeselectAllLeft,
          }),
        ),
        m(
          '.pf-join-column-list',
          leftColumns.length === 0
            ? m(DataExplorerEmptyState, {
                icon: 'cable',
                title: 'Connect left source',
              })
            : leftColumns.map((col, i) =>
                this.renderColumnRow(
                  col,
                  i,
                  onLeftColumnToggle,
                  onLeftColumnAlias,
                ),
              ),
        ),
      ]),
      // Right columns section
      m('.pf-join-column-section', [
        m(
          '.pf-join-column-section__header',
          m(
            'h4',
            `${rightAlias} (${rightSelectedCount} / ${rightColumns.length} selected)`,
          ),
          m(SelectDeselectAllButtons, {
            onSelectAll: onSelectAllRight,
            onDeselectAll: onDeselectAllRight,
          }),
        ),
        m(
          '.pf-join-column-list',
          rightColumns.length === 0
            ? m(DataExplorerEmptyState, {
                icon: 'cable',
                title: 'Connect right source',
              })
            : rightColumns.map((col, i) =>
                this.renderColumnRow(
                  col,
                  i,
                  onRightColumnToggle,
                  onRightColumnAlias,
                ),
              ),
        ),
      ]),
    ]);
  }
}
