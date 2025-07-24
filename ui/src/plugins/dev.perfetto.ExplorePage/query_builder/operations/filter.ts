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

// A type representing the right hand side of a filter operation.
export type FilterValue = string[] | number[];

// A filter is an object which represents a single "WHERE" clause in a SQL
// query.
// This is a "complete" filter, which has all the necessary fields to be
// translated into a SQL condition.
export interface Filter {
  readonly col: ColumnInfo;
  readonly op: FilterOp;
  readonly value?: FilterValue;
}

/**
 * This interface represents a "filter" which is still being constructed in the
 * UI. Some of its fields may be missing or invalid.
 * This is useful to distinguish between a "complete" filter and one which is
 * still being built.
 */
export interface WipFilter {
  raw?: string;
  col?: ColumnInfo;
  op?: FilterOp;
  value?: FilterValue;
  isEditing?: boolean;
}

/**
 * Attributes for the FilterOperation component.
 */
export interface FilterAttrs {
  sourceCols: ColumnInfo[];
  filters: WipFilter[];
}

function editFilter(filters: WipFilter[], index: number) {
  // Clean up invalid filters which are not being edited.
  for (let i = filters.length - 1; i >= 0; i--) {
    if (i !== index && !isFilterValid(filters[i])) {
      filters.splice(i, 1);
    }
  }

  // Ensure only one filter is in editing mode.
  for (let i = 0; i < filters.length; i++) {
    filters[i].isEditing = i === index;
  }
}

/**
 * A component which allows the user to add, remove and edit filters.
 */
export class FilterOperation implements m.ClassComponent<FilterAttrs> {
  private error?: string;

  view({attrs}: m.CVnode<FilterAttrs>) {
    const {filters, sourceCols} = attrs;

    const editor =
      filters.findIndex((f) => f.isEditing) === -1
        ? undefined
        : m(FilterEditor, {
            filter: filters.find((f) => f.isEditing)!,
            sourceCols,
            onUpdate: (newFilter) => {
              const index = filters.findIndex((f) => f.isEditing);
              filters[index] = newFilter;
            },
            onRemove: () => {
              const index = filters.findIndex((f) => f.isEditing);
              filters.splice(index, 1);
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
                  if (!isFilterValid(filter)) {
                    if (filter.col === undefined) {
                      this.error = `Column not found in "${text}"`;
                    } else if (filter.op === undefined) {
                      this.error = `Operator not found in "${text}"`;
                    } else {
                      this.error = `Filter value is missing in "${text}"`;
                    }
                    return;
                  }
                  this.error = undefined;
                  filters.push(filter);
                  target.value = '';
                }
              }
            },
          }),
        ),
        this.error && m('.pf-error-message', this.error),
        m(
          ChipBar,
          filters.map((filter, index) => {
            if (filter.isEditing) {
              return;
            }

            const isValid = isFilterValid(filter);
            const label = isValid
              ? `${filter.col!.name} ${filter.op!.displayName} ${
                  filter.value ?? ''
                }`
              : filter.raw;

            if (label === undefined) {
              return;
            }

            return m(Chip, {
              label,
              rounded: true,
              intent: isValid ? Intent.Primary : Intent.None,
              onclick: () => {
                editFilter(filters, index);
              },
            });
          }),
          m(Chip, {
            icon: 'add',
            rounded: true,
            intent: Intent.Primary,
            onclick: () => {
              const editingIndex = filters.findIndex((f) => f.isEditing);

              // If an editor is already open for a new filter, remove it.
              if (editingIndex > -1 && !isFilterValid(filters[editingIndex])) {
                filters.splice(editingIndex, 1);
              } else {
                // Otherwise, add a new filter and start editing it.
                // This will also close any other existing editor.
                filters.push({isEditing: true});
                editFilter(filters, filters.length - 1);
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
  filter: WipFilter;
  sourceCols: ColumnInfo[];
  onUpdate: (filter: WipFilter) => void;
  onRemove: () => void;
}

// A component which allows the user to edit a single filter.
class FilterEditor implements m.ClassComponent<FilterEditorAttrs> {
  view({attrs}: m.CVnode<FilterEditorAttrs>): m.Children {
    const {filter, sourceCols, onUpdate, onRemove} = attrs;

    const {col, op} = filter;
    const valueRequired = isValueRequired(op);
    const isValid = isFilterValid(filter);
    const colOptions = sourceCols
      .filter((c) => c.checked)
      .map(({name}) => {
        return m('option', {value: name, selected: name === col?.name}, name);
      });

    const opOptions = ALL_FILTER_OPS.map((op) => {
      return m(
        'option',
        {
          value: op.key,
          selected: op === filter.op,
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
              const selectedColumn = sourceCols.find(
                (c) => c.name === target.value,
              );
              onUpdate({...filter, col: selectedColumn});
            },
          },
          m('option', {disabled: true, selected: col === undefined}, 'Column'),
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
              const newFilter = {...filter, op: newOp};
              if (!isValueRequired(newOp)) {
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
            value: filter.value?.join(','),
            oninput: (e: Event) => {
              const target = e.target as HTMLInputElement;
              const value = parseFilterValue(target.value);
              onUpdate({...filter, value});
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
export function isFilterValid(filter: WipFilter): boolean {
  const {col, op, value} = filter;

  return (
    col !== undefined &&
    op !== undefined &&
    (!isValueRequired(op) || (value !== undefined && value.length > 0))
  );
}

// Tries to parse a filter from a raw string. This is a best-effort parser
// for simple filters and does not support complex values with spaces or quotes.
// TODO(mayzner): Improve this parser to handle more complex cases, such as
// quoted strings, escaped characters, or operators within values.
function fromString(text: string, sourceCols: ColumnInfo[]): WipFilter {
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
      return {raw: text, col};
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

  if (isValueRequired(op) && (value === undefined || value.length === 0)) {
    // Value is required but not found or empty
    return {raw: text, col, op};
  }

  return {raw: text, col, op, value};
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
function parseFilterValue(text: string): FilterValue | undefined {
  const values = text
    .split(',')
    .map((s) => s.trim())
    .filter((s) => s !== '');

  if (values.length === 0) {
    return undefined;
  }

  // If all values look like numbers, treat them as numbers, otherwise treat
  // them all as strings.
  if (values.every((v) => v !== '' && !isNaN(Number(v)))) {
    return values.map(Number);
  } else {
    return values;
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

/**
 * Converts a Filter object to its protobuf representation.
 * @param filter The filter to convert.
 * @returns The protobuf representation of the filter.
 */
export function FilterToProto(
  filter: Filter,
): protos.PerfettoSqlStructuredQuery.Filter {
  const {col, op, value} = filter;
  const result = new protos.PerfettoSqlStructuredQuery.Filter();
  result.columnName = col.name;
  result.op = op.proto;

  if (value && value.length > 0) {
    if (typeof value[0] === 'string') {
      result.stringRhs = value as string[];
    } else if (col.type === 'long' || col.type === 'int') {
      result.int64Rhs = value as number[];
    } else {
      result.doubleRhs = value as number[];
    }
  }

  return result;
}
