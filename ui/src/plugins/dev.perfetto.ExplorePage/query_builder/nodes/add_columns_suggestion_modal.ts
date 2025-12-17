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
import {Select} from '../../../../widgets/select';
import {Callout} from '../../../../widgets/callout';
import {Form, FormSection} from '../../../../widgets/form';
import {
  TableDescription,
  DataExplorerEmptyState,
  IssueList,
  OutlinedFieldReadOnly,
} from '../widgets';
import {JoinSourceCard} from '../join_widgets';
import {ColumnInfo, columnInfoFromSqlColumn} from '../column_info';
import {
  SqlTable,
  SqlColumn,
} from '../../../dev.perfetto.SqlModules/sql_modules';

/**
 * Modal for suggesting and connecting a new table to join
 * Shown when no secondary input is connected yet
 */
export interface AddColumnsSuggestionModalAttrs {
  // Data
  readonly suggestions: Array<{
    colName: string;
    suggestedTable: string;
    targetColumn: string;
  }>;
  readonly sourceCols: ColumnInfo[];
  readonly selectedTable?: string;
  readonly selectedColumns: string[];
  readonly suggestionAliases?: Map<string, string>;

  // Callbacks
  readonly getTable: (tableName: string) => SqlTable | undefined;
  readonly getJoinColumnErrors: (
    selectedColumns: string[],
  ) => Array<{column: string; error: string}>;
  readonly onTableSelect: (tableName: string | undefined) => void;
  readonly onColumnToggle: (columnName: string, checked: boolean) => void;
  readonly onColumnAlias: (columnName: string, alias: string) => void;
}

export class AddColumnsSuggestionModal
  implements m.ClassComponent<AddColumnsSuggestionModalAttrs>
{
  oncreate({attrs}: m.VnodeDOM<AddColumnsSuggestionModalAttrs>) {
    // Auto-select if there's only one suggestion and nothing is selected yet
    if (attrs.suggestions.length === 1 && !attrs.selectedTable) {
      attrs.onTableSelect(attrs.suggestions[0].suggestedTable);
    }
  }

  view({attrs}: m.Vnode<AddColumnsSuggestionModalAttrs>) {
    const {
      suggestions,
      sourceCols,
      selectedTable,
      selectedColumns,
      suggestionAliases,
      getTable,
      getJoinColumnErrors,
      onTableSelect,
      onColumnToggle,
      onColumnAlias,
    } = attrs;

    if (suggestions.length === 0) {
      return m(
        Form,
        m(
          'p',
          'No JOINID columns found in your data. You can still connect any node to the left port.',
        ),
      );
    }

    const hasOnlyOneSuggestion = suggestions.length === 1;
    const selectedSuggestion = suggestions.find(
      (s) => s.suggestedTable === selectedTable,
    );
    const tableInfo = selectedTable ? getTable(selectedTable) : undefined;

    // Create ColumnInfo objects from table columns with checked state
    const tableColsWithChecked: ColumnInfo[] = tableInfo
      ? tableInfo.columns.map((col: SqlColumn) => ({
          ...columnInfoFromSqlColumn(col),
          checked: selectedColumns.includes(col.name),
          alias: suggestionAliases?.get(col.name),
        }))
      : [];

    return m(
      '.pf-join-modal-layout',
      m(
        '.pf-join-modal-controls',
        m(
          Form,
          selectedSuggestion &&
            selectedColumns.length === 0 &&
            m(
              Callout,
              {icon: 'info'},
              'Select at least one column to add from the joined table.',
            ),
          selectedSuggestion &&
            selectedColumns.length > 0 &&
            m(IssueList, {
              icon: 'error',
              title: 'Column name conflicts:',
              items: getJoinColumnErrors(selectedColumns).map(
                (err) => err.error,
              ),
            }),
          // Only show selector if there are multiple suggestions
          !hasOnlyOneSuggestion &&
            m(FormSection, {label: 'Select Table to Join'}, [
              m(
                Select,
                {
                  onchange: (e: Event) => {
                    const value = (e.target as HTMLSelectElement).value;
                    onTableSelect(value || undefined);
                  },
                },
                m(
                  'option',
                  {value: '', selected: !selectedTable},
                  'Choose a table',
                ),
                suggestions.map((s) =>
                  m(
                    'option',
                    {
                      value: s.suggestedTable,
                      selected: s.suggestedTable === selectedTable,
                    },
                    `${s.suggestedTable} (on ${s.colName})`,
                  ),
                ),
              ),
            ]),
          // Show table name as read-only OutlinedField when there's only one option
          hasOnlyOneSuggestion &&
            selectedSuggestion &&
            m(OutlinedFieldReadOnly, {
              label: 'Joining Table',
              value: `${selectedSuggestion.suggestedTable} (on ${selectedSuggestion.colName})`,
            }),
          selectedSuggestion &&
            m(JoinSourceCard, {
              label: selectedTable ?? 'Source',
              columns: tableColsWithChecked,
              otherSideColumns: sourceCols.map((col) => ({
                ...col,
                checked: true,
              })),
              selectedColumn: selectedSuggestion.targetColumn,
              joinColumnDisabled: true,
              onColumnChange: () => {
                // Join column is auto-detected, no change needed
              },
              onColumnToggle: (index: number, checked: boolean) => {
                if (!tableInfo) return;
                const colName = tableInfo.columns[index].name;
                onColumnToggle(colName, checked);
              },
              onColumnAlias: (index: number, alias: string) => {
                if (!tableInfo) return;
                const colName = tableInfo.columns[index].name;
                onColumnAlias(colName, alias);
              },
            }),
        ),
      ),
      m(
        '.pf-join-modal-info',
        tableInfo
          ? m(TableDescription, {table: tableInfo})
          : m(DataExplorerEmptyState, {
              icon: 'table',
              title: 'Table information will appear here',
            }),
      ),
    );
  }
}
