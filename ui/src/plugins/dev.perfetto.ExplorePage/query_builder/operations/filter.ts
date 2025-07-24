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
import {ColumnInfo} from '../column_info';
import {Button} from '../../../../widgets/button';
import {Select} from '../../../../widgets/select';
import {TextInput} from '../../../../widgets/text_input';
import protos from '../../../../protos';
import {Chip, ChipBar} from '../../../../widgets/chip';
import {Intent} from '../../../../widgets/common';
import {
  FilterDefinition,
  FilterValue,
} from '../../../../components/widgets/data_grid/common';
import {SqlValue} from '../../../../trace_processor/query_result';

// Our internal representation for a filter in the UI.
// It can be a raw string, a partially built filter, or a complete one.
export interface UIFilter {
  column?: string;
  op?: FilterValue['op'] | 'is null' | 'is not null';
  value?: SqlValue;
  isEditing?: boolean;
  raw?: string;
}

/**
 * Attributes for the FilterOperation component.
 */
export interface FilterAttrs {
  sourceCols: ColumnInfo[];
  filters: FilterDefinition[];
  onFiltersChanged?: (filters: FilterDefinition[]) => void;
}

export class FilterOperation implements m.ClassComponent<FilterAttrs> {
  private error?: string;
  private uiFilters: UIFilter[] = [];
  private parentOnFiltersChanged?: (filters: FilterDefinition[]) => void;

  oninit({attrs}: m.Vnode<FilterAttrs>) {
    this.uiFilters = attrs.filters.map(filterDefinitionToUiFilter);
    this.parentOnFiltersChanged = attrs.onFiltersChanged;
  }

  onbeforeupdate({attrs}: m.Vnode<FilterAttrs>) {
    // If we are not in editing mode, sync with the parent.
    if (this.uiFilters.every((f) => !f.isEditing)) {
      this.uiFilters = attrs.filters.map(filterDefinitionToUiFilter);
    }
    this.parentOnFiltersChanged = attrs.onFiltersChanged;
  }

  private setFilters(nextFilters: UIFilter[]) {
    this.uiFilters = nextFilters;
    // Only notify the parent of "stable" changes, i.e. when not editing.
    if (this.uiFilters.every((f) => !f.isEditing)) {
      this.parentOnFiltersChanged?.(
        this.uiFilters
          .map(uiFilterToFilterDefinition)
          .filter((f): f is FilterDefinition => f !== undefined),
      );
    }
  }

  view({attrs}: m.CVnode<FilterAttrs>) {
    const {sourceCols} = attrs;

    const editor =
      this.uiFilters.findIndex((f) => f.isEditing) === -1
        ? undefined
        : m(FilterEditor, {
            filter: this.uiFilters.find((f) => f.isEditing)!,
            sourceCols,
            onUpdate: (newFilter) => {
              const index = this.uiFilters.findIndex((f) => f.isEditing);
              const nextFilters = this.uiFilters.map((f, i) =>
                i === index ? newFilter : f,
              );
              this.setFilters(nextFilters);
            },
            onRemove: () => {
              const index = this.uiFilters.findIndex((f) => f.isEditing);
              const nextFilters = this.uiFilters.filter((_, i) => i !== index);
              this.setFilters(nextFilters);
            },
          });

    return m('.pf-query-operations', [
      m('.section', [
        m(
          '.pf-filters-header',
          m('h2.pf-filters-title', 'Filters'),
          m(TextInput, {
            placeholder: 'eg. ts > 1000',
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
                  this.setFilters([...this.uiFilters, filter]);
                  target.value = '';
                }
              }
            },
          }),
        ),
        this.error && m('.pf-error-message', this.error),
        m(
          ChipBar,
          this.uiFilters.map((filter, index) => {
            if (filter.isEditing) {
              return;
            }

            const isComplete = isFilterDefinitionValid(filter);
            const label = isComplete
              ? `${filter.column} ${filter.op} ${
                  'value' in filter ? filter.value : ''
                }`
              : filter.raw;

            if (label === undefined) {
              return;
            }

            return m(Chip, {
              label,
              rounded: true,
              intent: isComplete ? Intent.Primary : Intent.None,
              onclick: () => {
                const filterToEdit = this.uiFilters[index];
                let nextFilters = this.uiFilters.filter(
                  (f, i) => i === index || isFilterDefinitionValid(f),
                );
                const newIndex = nextFilters.indexOf(filterToEdit);
                nextFilters = nextFilters.map((f, i) => {
                  return {...f, isEditing: i === newIndex};
                });
                this.setFilters(nextFilters);
              },
            });
          }),
          m(Chip, {
            icon: 'add',
            rounded: true,
            intent: Intent.Primary,
            onclick: () => {
              const editingIndex = this.uiFilters.findIndex((f) => f.isEditing);

              if (
                editingIndex > -1 &&
                !isFilterDefinitionValid(this.uiFilters[editingIndex])
              ) {
                const nextFilters = this.uiFilters.filter(
                  (_, i) => i !== editingIndex,
                );
                this.setFilters(nextFilters);
              } else {
                const nextFilters = [
                  ...this.uiFilters.filter(isFilterDefinitionValid),
                  {isEditing: true},
                ];
                this.setFilters(nextFilters);
              }
            },
          }),
        ),
        editor && m('.pf-filter-editor-box', editor),
      ]),
    ]);
  }
}

interface FilterEditorAttrs {
  filter: UIFilter;
  sourceCols: ColumnInfo[];
  onUpdate: (filter: UIFilter) => void;
  onRemove: () => void;
}

// A component which allows the user to edit a single filter.
class FilterEditor implements m.ClassComponent<FilterEditorAttrs> {
  view({attrs}: m.CVnode<FilterEditorAttrs>): m.Children {
    const {filter, sourceCols, onUpdate, onRemove} = attrs;

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
      '.pf-filter-editor',
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
                delete newFilter.value;
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
              onUpdate({...filter, value: value});
            },
          }),
        m(Button, {
          className: 'delete-button',
          icon: 'delete',
          onclick: onRemove,
        }),
        m(Button, {
          label: 'Done',
          className: 'is-primary',
          disabled: !isValid,
          onclick: () => {
            onUpdate({...filter, isEditing: false});
          },
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
  // Sort operators by length descending to match "is not null" before "is null".
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
      return {raw: text, column: col.name};
    }
    return {raw: text};
  }

  const [, colName, opName, valueText] = match;

  const col = sourceCols.find(
    (c) => c.name.toLowerCase() === colName.toLowerCase(),
  );
  if (col === undefined) {
    return {raw: text};
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
      raw: text,
      column: col.name,
      op: op.displayName as UIFilter['op'],
    };
  }

  const result: UIFilter = {
    raw: text,
    column: col.name,
    op: op.displayName as UIFilter['op'],
  };

  if (value !== undefined) {
    result.value = value;
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

function uiFilterToFilterDefinition(
  uiFilter: UIFilter,
): FilterDefinition | undefined {
  if (isFilterDefinitionValid(uiFilter)) {
    const {isEditing: _isEditing, raw: _raw, ...def} = uiFilter;
    return def as FilterDefinition;
  }
  return undefined;
}

export function filterDefinitionToUiFilter(def: FilterDefinition): UIFilter {
  return {...def, isEditing: false};
}
