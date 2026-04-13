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
import {Callout} from '../../../../widgets/callout';
import {Form, FormSection} from '../../../../widgets/form';
import {IssueList, OutlinedField} from '../widgets';
import {JoinSourceCard} from '../join_widgets';
import {ColumnInfo} from '../column_info';

/**
 * Modal for configuring column selection from an already-connected join source
 * Shown when a secondary input is already connected
 */
export interface AddColumnsConfigurationModalAttrs {
  // Data
  readonly sourceCols: ColumnInfo[];
  readonly rightCols: ColumnInfo[];
  readonly leftColumn?: string;
  readonly rightColumn?: string;
  readonly selectedColumns: string[];
  readonly columnAliases?: Map<string, string>;

  // Callbacks
  readonly getJoinColumnErrors: (
    selectedColumns: string[],
  ) => Array<{column: string; error: string}>;
  readonly onLeftColumnChange: (columnName: string) => void;
  readonly onRightColumnChange: (columnName: string) => void;
  readonly onColumnToggle: (columnName: string, checked: boolean) => void;
  readonly onColumnAlias: (columnName: string, alias: string) => void;
}

export class AddColumnsConfigurationModal
  implements m.ClassComponent<AddColumnsConfigurationModalAttrs>
{
  view({attrs}: m.Vnode<AddColumnsConfigurationModalAttrs>) {
    const {
      sourceCols,
      rightCols,
      leftColumn,
      rightColumn,
      selectedColumns,
      columnAliases,
      getJoinColumnErrors,
      onLeftColumnChange,
      onRightColumnChange,
      onColumnToggle,
      onColumnAlias,
    } = attrs;

    const noColumnsSelected = selectedColumns.length === 0;
    const missingJoinColumns = !leftColumn || !rightColumn;

    // Create rightCols with checked state based on selectedColumns
    const rightColsWithChecked = rightCols.map((col) => ({
      ...col,
      checked: selectedColumns.includes(col.column.name),
      alias: columnAliases?.get(col.column.name),
    }));

    return m(
      Form,
      missingJoinColumns &&
        m(
          Callout,
          {icon: 'warning'},
          'Select join columns from both sources to enable the join.',
        ),
      noColumnsSelected &&
        m(
          Callout,
          {icon: 'info'},
          'Select at least one column to add from the joined source.',
        ),
      selectedColumns.length > 0 &&
        m(IssueList, {
          icon: 'error',
          title: 'Column name conflicts:',
          items: getJoinColumnErrors(selectedColumns).map((err) => err.error),
        }),
      // Primary join column selector
      m(FormSection, {label: 'Join Condition'}, [
        m(
          OutlinedField,
          {
            label: 'Primary Join Column',
            value: leftColumn || '',
            onchange: (e: Event) => {
              const target = e.target as HTMLSelectElement;
              onLeftColumnChange(target.value);
            },
          },
          [
            m(
              'option',
              {
                value: '',
                selected: !leftColumn,
                disabled: true,
              },
              'Select column',
            ),
            ...sourceCols.map((col) =>
              m(
                'option',
                {
                  value: col.column.name,
                  selected: col.column.name === leftColumn,
                },
                col.column.name,
              ),
            ),
          ],
        ),
      ]),
      // Source card with join column and column selection
      m(JoinSourceCard, {
        label: 'Source',
        columns: rightColsWithChecked,
        otherSideColumns: sourceCols.map((col) => ({
          ...col,
          checked: true, // All primary columns are considered for conflict detection
        })),
        selectedColumn: rightColumn,
        onColumnChange: (columnName: string) => {
          onRightColumnChange(columnName);
        },
        onColumnToggle: (index: number, checked: boolean) => {
          const colName = rightCols[index].column.name;
          onColumnToggle(colName, checked);
        },
        onColumnAlias: (index: number, alias: string) => {
          const colName = rightCols[index].column.name;
          onColumnAlias(colName, alias);
        },
      }),
    );
  }
}
