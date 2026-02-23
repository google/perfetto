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
import {Select} from '../../../../widgets/select';
import {TextInput} from '../../../../widgets/text_input';
import {Form, FormSection} from '../../../../widgets/form';
import {FunctionList, FunctionWithModule} from '../function_list';
import {FunctionArgBinding, NewColumn} from './add_columns_types';
import {SqlModules} from '../../../dev.perfetto.SqlModules/sql_modules';
import {ColumnInfo} from '../column_info';
import {
  parsePerfettoSqlTypeFromString,
  isQuantitativeType,
} from '../../../../trace_processor/perfetto_sql_type';

/**
 * The step in the function modal flow
 */
export type FunctionModalStep = 'select' | 'configure';

/**
 * State for the function modal
 */
export interface FunctionModalState {
  step: FunctionModalStep;
  searchQuery: string;
  selectedFunctionWithModule?: FunctionWithModule;
  argBindings: FunctionArgBinding[];
  columnName: string;
}

/**
 * Creates initial state for the function modal
 */
export function createFunctionModalState(
  isEditing: boolean,
  existingColumn: NewColumn | undefined,
  sqlModules: SqlModules | undefined,
): FunctionModalState {
  const state: FunctionModalState = {
    step: isEditing ? 'configure' : 'select',
    searchQuery: '',
    selectedFunctionWithModule: undefined,
    argBindings: [],
    columnName: '',
  };

  // If editing, initialize from existing column
  if (isEditing && existingColumn && sqlModules) {
    const modules = sqlModules.listModules();
    for (const module of modules) {
      const fn = module.functions.find(
        (f) => f.name === existingColumn.functionName,
      );
      if (fn) {
        state.selectedFunctionWithModule = {fn, module};
        state.argBindings = [...(existingColumn.functionArgs ?? [])];
        state.columnName = existingColumn.name ?? '';
        break;
      }
    }
  }

  return state;
}

/**
 * Builds the expression string from selected function and argument bindings
 */
export function buildFunctionExpression(
  selectedFunctionWithModule: FunctionWithModule | undefined,
  argBindings: FunctionArgBinding[],
): string {
  if (!selectedFunctionWithModule) return '';
  const fn = selectedFunctionWithModule.fn;
  const args = fn.args
    .map((arg) => {
      const binding = argBindings.find((b) => b.argName === arg.name);
      if (!binding) return '';
      return binding.value;
    })
    .join(', ');
  return `${fn.name}(${args})`;
}

/**
 * Gets columns filtered by type compatibility with the argument type
 */
export function getColumnsForArgType(
  argType: string,
  sourceCols: ColumnInfo[],
): string[] {
  const argTypeResult = parsePerfettoSqlTypeFromString({type: argType});
  if (!argTypeResult.ok) {
    // Unknown arg type - show all columns
    return sourceCols.map((col) => col.column.name);
  }
  const parsedArgType = argTypeResult.value;
  const argIsQuantitative = isQuantitativeType(parsedArgType);
  const argIsString = parsedArgType.kind === 'string';

  return sourceCols
    .filter((col) => {
      const colType = col.column.type;
      if (colType === undefined) {
        // Unknown column type - include it
        return true;
      }
      if (argIsQuantitative) {
        return isQuantitativeType(colType);
      }
      if (argIsString) {
        return colType.kind === 'string';
      }
      // For other types (like bytes), show all columns
      return true;
    })
    .map((col) => col.column.name);
}

/**
 * Validates whether the function modal state is valid for submission
 */
export function isFunctionModalValid(
  state: FunctionModalState,
  getColumnNameError: (name: string) => string | undefined,
): boolean {
  if (!state.selectedFunctionWithModule) return false;
  if (!state.columnName.trim()) return false;
  if (getColumnNameError(state.columnName.trim())) return false;

  // Check all required args have values
  for (const arg of state.selectedFunctionWithModule.fn.args) {
    const binding = state.argBindings.find((b) => b.argName === arg.name);
    if (!binding || !binding.value.trim()) return false;
  }
  return true;
}

/**
 * Creates a NewColumn from the function modal state
 */
export function createFunctionColumn(
  state: FunctionModalState,
): NewColumn | undefined {
  if (!state.selectedFunctionWithModule) return undefined;

  return {
    type: 'function',
    expression: buildFunctionExpression(
      state.selectedFunctionWithModule,
      state.argBindings,
    ),
    name: state.columnName.trim(),
    module: state.selectedFunctionWithModule.module.includeKey,
    functionName: state.selectedFunctionWithModule.fn.name,
    functionArgs: state.argBindings,
    sqlType: state.selectedFunctionWithModule.fn.returnType,
  };
}

/**
 * Attributes for the FunctionSelectStep component
 */
export interface FunctionSelectStepAttrs {
  sqlModules: SqlModules;
  searchQuery: string;
  selectedFunction?: string;
  onSearchQueryChange: (query: string) => void;
  onFunctionSelect: (fn: FunctionWithModule) => void;
}

/**
 * Component for the function selection step
 */
export class FunctionSelectStep
  implements m.ClassComponent<FunctionSelectStepAttrs>
{
  view({attrs}: m.CVnode<FunctionSelectStepAttrs>) {
    return m(
      '.pf-exp-node-explorer-help',
      m(FunctionList, {
        sqlModules: attrs.sqlModules,
        onFunctionClick: attrs.onFunctionSelect,
        searchQuery: attrs.searchQuery,
        onSearchQueryChange: attrs.onSearchQueryChange,
        autofocus: true,
        selectedFunction: attrs.selectedFunction,
      }),
    );
  }
}

/**
 * Attributes for the FunctionConfigureStep component
 */
export interface FunctionConfigureStepAttrs {
  selectedFunctionWithModule: FunctionWithModule;
  argBindings: FunctionArgBinding[];
  columnName: string;
  columnNameError?: string;
  sourceCols: ColumnInfo[];
  onArgBindingChange: (argIndex: number, binding: FunctionArgBinding) => void;
  onColumnNameChange: (name: string) => void;
}

/**
 * Component for the function configuration step
 */
export class FunctionConfigureStep
  implements m.ClassComponent<FunctionConfigureStepAttrs>
{
  view({attrs}: m.CVnode<FunctionConfigureStepAttrs>) {
    const {
      selectedFunctionWithModule,
      argBindings,
      columnName,
      columnNameError,
      sourceCols,
      onArgBindingChange,
      onColumnNameChange,
    } = attrs;
    const fn = selectedFunctionWithModule.fn;
    const expression = buildFunctionExpression(
      selectedFunctionWithModule,
      argBindings,
    );

    return m(
      Form,
      // Function info
      m(
        '.pf-function-info',
        fn.description && m('p', fn.description),
        m('p', m('strong', 'Returns: '), fn.returnType),
      ),
      // Argument bindings
      fn.args.length > 0 &&
        m(FormSection, {label: 'Arguments'}, [
          ...fn.args.map((arg, argIndex) => {
            const binding = argBindings[argIndex];
            const columnsForArg = getColumnsForArgType(arg.type, sourceCols);

            return m(
              '.pf-function-arg',
              {key: arg.name},
              m(
                'label',
                `${arg.name} (${arg.type}): `,
                arg.description && m('small', arg.description),
              ),
              m(
                Select,
                {
                  onchange: (e: Event) => {
                    const value = (e.target as HTMLSelectElement).value;
                    if (value === '__expression__') {
                      onArgBindingChange(argIndex, {
                        argName: arg.name,
                        value: '',
                        isCustomExpression: true,
                      });
                    } else {
                      onArgBindingChange(argIndex, {
                        argName: arg.name,
                        value,
                        isCustomExpression: false,
                      });
                    }
                  },
                },
                m('option', {value: ''}, 'Select column...'),
                columnsForArg.map((col) =>
                  m(
                    'option',
                    {
                      value: col,
                      selected:
                        !binding?.isCustomExpression && binding?.value === col,
                    },
                    col,
                  ),
                ),
                m(
                  'option',
                  {
                    value: '__expression__',
                    selected: binding?.isCustomExpression,
                  },
                  'Custom expression...',
                ),
              ),
              binding?.isCustomExpression &&
                m(TextInput, {
                  placeholder: 'e.g., dur / 1e6, ts + 100, name',
                  value: binding.value,
                  oninput: (e: Event) => {
                    onArgBindingChange(argIndex, {
                      argName: arg.name,
                      value: (e.target as HTMLInputElement).value,
                      isCustomExpression: true,
                    });
                  },
                }),
            );
          }),
        ]),
      // Column name
      m(FormSection, {label: 'Column Name'}, [
        m(TextInput, {
          placeholder: 'Enter column name',
          value: columnName,
          oninput: (e: Event) => {
            onColumnNameChange((e.target as HTMLInputElement).value);
          },
        }),
        columnNameError && m('.pf-error-text', columnNameError),
      ]),
      // Expression preview
      m(FormSection, {label: 'Expression Preview'}, m('code', expression)),
    );
  }
}
