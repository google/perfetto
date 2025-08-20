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
import {
  FilterDefinition,
  FilterValue,
} from '../../../../components/widgets/data_grid/common';
import {Button} from '../../../../widgets/button';
import {Chip, ChipBar} from '../../../../widgets/chip';
import {Intent} from '../../../../widgets/common';
import {Select} from '../../../../widgets/select';
import {TextInput} from '../../../../widgets/text_input';
import {SqlValue} from '../../../../trace_processor/query_result';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';

// Partial representation of FilterDefinition used in the UI.
export interface UIFilter {
  readonly column?: string;
  readonly op?: FilterValue['op'] | 'is null' | 'is not null';
  readonly value?: SqlValue;
}

/**
 * Attributes for the FilterOperation component.
 */
export interface FilterAttrs {
  readonly sourceCols: ColumnInfo[];
  readonly filters: ReadonlyArray<FilterDefinition>;
  readonly onFiltersChanged?: (
    filters: ReadonlyArray<FilterDefinition>,
  ) => void;
}

export class FilterOperation implements m.ClassComponent<FilterAttrs> {
  private error?: string;
  private uiFilters: UIFilter[] = [];
  private editingFilter?: UIFilter;

  oncreate({attrs}: m.Vnode<FilterAttrs>) {
    this.uiFilters = [...attrs.filters];
  }

  onbeforeupdate({attrs}: m.Vnode<FilterAttrs>) {
    // If we are not in editing mode, sync with the parent.
    if (this.editingFilter === undefined) {
      this.uiFilters = [...attrs.filters];
    }
  }

  private setFilters(
    nextFilters: UIFilter[],
    attrs: FilterAttrs,
    editing?: UIFilter,
  ) {
    this.uiFilters = nextFilters;
    this.editingFilter = editing;

    // Only notify the parent of "stable" changes, i.e. when not editing.
    if (this.editingFilter === undefined) {
      attrs.onFiltersChanged?.(this.uiFilters.filter(isFilterDefinitionValid));
    }
    m.redraw();
  }

  view({attrs}: m.CVnode<FilterAttrs>) {
    const {sourceCols} = attrs;

    const editor =
      this.editingFilter === undefined
        ? undefined
        : m(FilterEditor, {
            filter: this.editingFilter,
            sourceCols,
            onUpdate: (newFilter) => {
              const index = this.uiFilters.indexOf(this.editingFilter!);
              const nextFilters = this.uiFilters.map((f, i) =>
                i === index ? newFilter : f,
              );
              this.setFilters(nextFilters, attrs, newFilter);
            },
            onRemove: () => {
              const nextFilters = this.uiFilters.filter(
                (f) => f !== this.editingFilter,
              );
              this.setFilters(nextFilters, attrs, undefined);
            },
            onDone: () => {
              this.setFilters(this.uiFilters, attrs, undefined);
            },
          });

    return m('.pf-exp-query-operations', [
      m('.pf-exp-section', [
        m(
          '.pf-exp-filters-header',
          m('h2.pf-exp-filters-title', 'Filters'),
          m(TextInput, {
            placeholder: 'e.g. ts > 1000',
            onkeydown: (e: KeyboardEvent) => {
              const target = e.target as HTMLInputElement;
              if (e.key === 'Enter') {
                const text = target.value;
                if (text.length > 0) {
                  const filter = fromString(text, sourceCols);
                  if (!isFilterDefinitionValid(filter)) {
                    if (filter.column === undefined) {
                      this.error = `Column not found in "${text}"`;
                    } else if (filter.op === undefined) {
                      this.error = `Operator not found in "${text}"`;
                    } else {
                      this.error = `Filter value is missing in "${text}"`;
                    }
                    m.redraw();
                    return;
                  }
                  this.error = undefined;
                  this.setFilters([...this.uiFilters, filter], attrs);
                  target.value = '';
                }
              }
            },
          }),
        ),
        this.error && m('.pf-exp-error-message', this.error),
        m(
          ChipBar,
          this.uiFilters.map((filter) => {
            const isComplete = isFilterDefinitionValid(filter);
            const label = isComplete
              ? `${filter.column} ${filter.op} ${
                  'value' in filter ? filter.value : ''
                }`
              : 'New Filter';

            return m(Chip, {
              label,
              rounded: true,
              intent: isComplete ? Intent.Primary : Intent.None,
              onclick: () => {
                // When we start editing a chip, we remove all other invalid
                // filters from the list.
                const nextFilters = this.uiFilters.filter(
                  (f) => f === filter || isFilterDefinitionValid(f),
                );
                this.setFilters(nextFilters, attrs, filter);
              },
            });
          }),
          m(Chip, {
            icon: 'add',
            rounded: true,
            intent: Intent.Primary,
            onclick: () => {
              if (
                this.editingFilter !== undefined &&
                !isFilterDefinitionValid(this.editingFilter)
              ) {
                const nextFilters = this.uiFilters.filter(
                  (f) => f !== this.editingFilter,
                );
                this.setFilters(nextFilters, attrs, undefined);
              } else {
                const newFilter = {};
                const nextFilters = [...this.uiFilters, newFilter];
                this.setFilters(nextFilters, attrs, newFilter);
              }
            },
          }),
        ),
        editor && m('.pf-exp-filter-editor-box', editor),
      ]),
    ]);
  }
}

interface FilterEditorAttrs {
  readonly filter: UIFilter;
  readonly sourceCols: ColumnInfo[];
  readonly onUpdate: (filter: UIFilter) => void;
  readonly onRemove: () => void;
  readonly onDone: () => void;
}

// A component which allows the user to edit a single filter.
class FilterEditor implements m.ClassComponent<FilterEditorAttrs> {
  view({attrs}: m.CVnode<FilterEditorAttrs>): m.Children {
    const {filter, sourceCols, onUpdate, onRemove, onDone} = attrs;

    const {column, op} = filter;
    const opObject = ALL_FILTER_OPS.find((o) => o.displayName === op);
    const valueRequired = isValueRequired(opObject);
    const isValid = isFilterDefinitionValid(filter);
    const colOptions = sourceCols
      .filter((c) => c.checked)
      .map(({name}) => {
        return m('option', {value: name, selected: name === column}, name);
      });

    const opOptions = ALL_FILTER_OPS.map((op) => {
      return m(
        'option',
        {
          value: op.key,
          selected: op.displayName === filter.op,
        },
        op.displayName,
      );
    });

    return m(
      '.pf-exp-filter-editor',
      {className: isValid ? 'is-valid' : 'is-invalid'},
      [
        m(
          Select,
          {
            onchange: (e: Event) => {
              const target = e.target as HTMLSelectElement;
              onUpdate({...filter, column: target.value});
            },
          },
          m(
            'option',
            {disabled: true, selected: column === undefined},
            'Column',
          ),
          colOptions,
        ),
        m(
          Select,
          {
            onchange: (e: Event) => {
              const target = e.target as HTMLSelectElement;
              const newOp = ALL_FILTER_OPS.find(
                (op) => op.key === target.value,
              );
              const newFilter: UIFilter = {
                ...filter,
                op: newOp?.displayName as UIFilter['op'],
              };
              if (newOp && !isValueRequired(newOp)) {
                delete (newFilter as {value?: SqlValue}).value;
              }
              onUpdate(newFilter);
            },
          },
          m('option', {disabled: true, selected: op === undefined}, 'Operator'),
          opOptions,
        ),
        valueRequired &&
          m(TextInput, {
            placeholder: 'Value',
            value: 'value' in filter ? String(filter.value) : '',
            oninput: (e: Event) => {
              const target = e.target as HTMLInputElement;
              const value = parseFilterValue(target.value);
              onUpdate({...filter, value});
            },
          }),
        m(Button, {
          className: 'pf-exp-delete-button',
          icon: 'delete',
          onclick: onRemove,
        }),
        m(Button, {
          label: 'Done',
          className: 'is-primary',
          disabled: !isValid,
          onclick: onDone,
        }),
      ],
    );
  }
}

/**
 * Check if a work-in-progress filter is valid and can be converted to a
 * proper Filter.
 * @param filter The filter to check.
 * @returns True if the filter is valid.
 */
export function isFilterDefinitionValid(
  filter: UIFilter,
): filter is FilterDefinition & UIFilter {
  const {column, op} = filter;

  if (column === undefined || op === undefined) {
    return false;
  }

  const opObject = ALL_FILTER_OPS.find((o) => o.displayName === op);

  if (opObject === undefined) {
    return false;
  }

  if (isValueRequired(opObject)) {
    if (!('value' in filter) || filter.value === undefined) {
      return false;
    }
  }

  return true;
}

// Tries to parse a filter from a raw string. This is a best-effort parser
// for simple filters and does not support complex values with spaces or quotes.
// TODO(mayzner): Improve this parser to handle more complex cases, such as
// quoted strings, escaped characters, or operators within values.
function fromString(text: string, sourceCols: ColumnInfo[]): UIFilter {
  // Sort operators by length descending to match "is not null" before "is
  // null".
  const ops = ALL_FILTER_OPS.slice().sort(
    (a, b) => b.displayName.length - a.displayName.length,
  );

  const opRegex = ops
    .map((op) => op.displayName.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'))
    .join('|');

  // A regex to capture the column, operator and value.
  // The value is optional to support operators like "IS NULL".
  const regex = new RegExp(`^(\\S+)\\s+(${opRegex})(?:\\s+(.*))?$`, 'i');
  const match = text.trim().match(regex);

  if (!match) {
    // If regex doesn't match, maybe it's just a column name.
    const col = sourceCols.find(
      (c) => c.name.toLowerCase() === text.trim().toLowerCase(),
    );
    if (col) {
      return {column: col.name};
    }
    return {};
  }

  const [, colName, opName, valueText] = match;

  const col = sourceCols.find(
    (c) => c.name.toLowerCase() === colName.toLowerCase(),
  );
  if (col === undefined) {
    return {};
  }

  // Find the exact operator object. We need to do a case-insensitive search.
  const op = ALL_FILTER_OPS.find(
    (o) => o.displayName.toLowerCase() === opName.toLowerCase(),
  );

  if (op === undefined) {
    throw new Error('Internal error: operator not found despite regex match');
  }

  const value = isValueRequired(op)
    ? parseFilterValue(valueText || '')
    : undefined;

  if (isValueRequired(op) && value === undefined) {
    // Value is required but not found or empty
    return {
      column: col.name,
      op: op.displayName as UIFilter['op'],
    };
  }

  const result: UIFilter = {
    column: col.name,
    op: op.displayName as UIFilter['op'],
  };

  if (value !== undefined) {
    (result as {value: SqlValue}).value = value;
  }

  return result;
}

function op(
  key: string,
  displayName: string,
  proto: protos.PerfettoSqlStructuredQuery.Filter.Operator,
): FilterOp {
  return {
    key,
    displayName,
    proto,
  };
}

/**
 * A "Filter Operation" - i.e. "equals", "less than", "glob", etc.
 * This is a plain object which represents the properties of a filter
 * operation.
 */
export interface FilterOp {
  readonly key: string;
  readonly displayName: string;
  readonly proto: protos.PerfettoSqlStructuredQuery.Filter.Operator;
}

function isValueRequired(op?: FilterOp): boolean {
  return op !== undefined && op.key !== 'IS_NULL' && op.key !== 'IS_NOT_NULL';
}

// Parses a comma-separated string of values into an array of strings or
// numbers.
// If all values can be parsed as numbers, it returns a number array.
// Otherwise, it returns a string array.
function parseFilterValue(text: string): SqlValue | undefined {
  const value = text.trim();
  if (value === '') return undefined;

  if (value !== '' && !isNaN(Number(value))) {
    return Number(value);
  } else {
    return value;
  }
}

/**
 * All available filter operations.
 */
export const ALL_FILTER_OPS: FilterOp[] = [
  op('EQUAL', '=', protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL),
  op(
    'NOT_EQUAL',
    '!=',
    protos.PerfettoSqlStructuredQuery.Filter.Operator.NOT_EQUAL,
  ),
  op(
    'LESS_THAN',
    '<',
    protos.PerfettoSqlStructuredQuery.Filter.Operator.LESS_THAN,
  ),
  op(
    'LESS_THAN_EQUAL',
    '<=',
    protos.PerfettoSqlStructuredQuery.Filter.Operator.LESS_THAN_EQUAL,
  ),
  op(
    'GREATER_THAN',
    '>',
    protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN,
  ),
  op(
    'GREATER_THAN_EQUAL',
    '>=',
    protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN_EQUAL,
  ),
  op(
    'IS_NULL',
    'is null',
    protos.PerfettoSqlStructuredQuery.Filter.Operator.IS_NULL,
  ),
  op(
    'IS_NOT_NULL',
    'is not null',
    protos.PerfettoSqlStructuredQuery.Filter.Operator.IS_NOT_NULL,
  ),
  op('GLOB', 'glob', protos.PerfettoSqlStructuredQuery.Filter.Operator.GLOB),
];
