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
import {OutlinedField, DataExplorerEmptyState} from './widgets';
import {classNames} from '../../../base/classnames';
import {Card} from '../../../widgets/card';

/**
 * Widget for displaying a join source as a card
 * Shows alias, join column selector, and list of other columns
 */
export interface JoinSourceCardAttrs {
  label: string; // e.g., "Left", "Right"
  alias: string;
  columns: ColumnInfo[];
  selectedColumn?: string;
  onAliasChange: (alias: string) => void;
  onColumnChange: (columnName: string) => void;
}

export class JoinSourceCard implements m.ClassComponent<JoinSourceCardAttrs> {
  view({attrs}: m.Vnode<JoinSourceCardAttrs>) {
    const {
      label,
      alias,
      columns,
      selectedColumn,
      onAliasChange,
      onColumnChange,
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

    // Separate selected column from other columns
    const otherColumns = columns.filter(
      (col) => col.column.name !== selectedColumn,
    );

    return m(
      Card,
      {className: 'pf-join-source-card'},
      m('.pf-join-source-card__header', label),
      m(
        '.pf-join-source-card__content',
        // Alias field
        m(OutlinedField, {
          label: 'Alias',
          value: alias,
          oninput: (e: Event) => {
            onAliasChange((e.target as HTMLInputElement).value);
          },
        }),
        // Join column selector
        m(
          OutlinedField,
          {
            label: 'Join Column',
            value: selectedColumn || '',
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
        // Other columns list
        otherColumns.length > 0 &&
          m(
            '.pf-join-source-card__columns',
            m('.pf-join-source-card__columns-label', 'Other columns:'),
            m(
              '.pf-join-source-card__columns-list',
              otherColumns.map((col) =>
                m('.pf-join-source-card__column-item', col.column.name),
              ),
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
  leftAlias: string;
  rightAlias: string;
  leftColumns: ColumnInfo[];
  rightColumns: ColumnInfo[];
  leftColumn?: string;
  rightColumn?: string;
  onLeftAliasChange: (alias: string) => void;
  onRightAliasChange: (alias: string) => void;
  onLeftColumnChange: (columnName: string) => void;
  onRightColumnChange: (columnName: string) => void;
}

export class JoinConditionSelector
  implements m.ClassComponent<JoinConditionSelectorAttrs>
{
  view({attrs}: m.Vnode<JoinConditionSelectorAttrs>) {
    const {
      leftLabel,
      rightLabel,
      leftAlias,
      rightAlias,
      leftColumns,
      rightColumns,
      leftColumn,
      rightColumn,
      onLeftAliasChange,
      onRightAliasChange,
      onLeftColumnChange,
      onRightColumnChange,
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
          alias: leftAlias,
          columns: leftColumns,
          selectedColumn: leftColumn,
          onAliasChange: onLeftAliasChange,
          onColumnChange: onLeftColumnChange,
        }),
        // Right source card
        m(JoinSourceCard, {
          label: rightLabel,
          alias: rightAlias,
          columns: rightColumns,
          selectedColumn: rightColumn,
          onAliasChange: onRightAliasChange,
          onColumnChange: onRightColumnChange,
        }),
      ),
      // Show visual preview of the join condition when both columns are selected
      hasValidJoin &&
        m(
          '.pf-join-condition-preview',
          m('code.pf-join-condition-preview__code', [
            m('span.pf-join-column-ref', `${leftAlias}.${leftColumn}`),
            m('span.pf-join-operator', ' = '),
            m('span.pf-join-column-ref', `${rightAlias}.${rightColumn}`),
          ]),
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
