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
import {Button} from '../../../../widgets/button';
import {Card} from '../../../../widgets/card';
import {Chip} from '../../../../widgets/chip';
import {Intent} from '../../../../widgets/common';
import {Select} from '../../../../widgets/select';
import {TextInput} from '../../../../widgets/text_input';
import {SqlValue} from '../../../../trace_processor/query_result';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import {Stack} from '../../../../widgets/stack';

interface FilterValue {
  readonly column: string;
  readonly op: '=' | '!=' | '<' | '<=' | '>' | '>=' | 'glob';
  readonly value: SqlValue;
}

interface FilterNull {
  readonly column: string;
  readonly op: 'is null' | 'is not null';
}

export type UIFilter = FilterValue | FilterNull;

/**
 * Attributes for the FilterOperation component.
 */
export interface FilterAttrs {
  readonly sourceCols: ColumnInfo[];
  readonly filters?: ReadonlyArray<UIFilter>;
  readonly onFiltersChanged?: (filters: ReadonlyArray<UIFilter>) => void;
  readonly onchange?: () => void;
}

export class FilterOperation implements m.ClassComponent<FilterAttrs> {
  private error?: string;
  private uiFilters: Partial<UIFilter>[] = [];
  private editingFilter?: Partial<UIFilter>;

  oncreate({attrs}: m.Vnode<FilterAttrs>) {
    this.uiFilters = [...(attrs.filters ?? [])];
  }

  onbeforeupdate({attrs}: m.Vnode<FilterAttrs>) {
    // If we are not in editing mode, sync with the parent.
    if (this.editingFilter === undefined) {
      this.uiFilters = [...(attrs.filters ?? [])];
    }
  }

  private setFilters(
    nextFilters: Partial<UIFilter>[],
    attrs: FilterAttrs,
    editing?: Partial<UIFilter>,
  ) {
    this.uiFilters = nextFilters;
    this.editingFilter = editing;

    // Only notify the parent of "stable" changes, i.e. when not editing.
    if (this.editingFilter === undefined) {
      attrs.onFiltersChanged?.(this.uiFilters.filter(isFilterDefinitionValid));
    }
    attrs.onchange?.();
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

    return m(
      '.pf-exp-query-operations',
      m(Card, {}, [
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
          Stack,
          {orientation: 'horizontal'},
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
          m(Button, {
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
                const newFilter: Partial<UIFilter> = {};
                const nextFilters = [...this.uiFilters, newFilter];
                this.setFilters(nextFilters, attrs, newFilter);
              }
            },
          }),
        ),
        editor && m('.pf-exp-editor-box', editor),
      ]),
    );
  }
}

interface FilterEditorAttrs {
  readonly filter: Partial<UIFilter>;
  readonly sourceCols: ColumnInfo[];
  readonly onUpdate: (filter: Partial<UIFilter>) => void;
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
      '.pf-exp-editor',
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
              const newFilter: Partial<UIFilter> = {
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
              if (value !== undefined) {
                onUpdate({...filter, value} as Partial<UIFilter>);
              } else {
                onUpdate(filter);
              }
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
  filter: Partial<UIFilter>,
): filter is UIFilter {
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
function fromString(text: string, sourceCols: ColumnInfo[]): Partial<UIFilter> {
  // Sort operators by length descending to match "is not null" before "is
  // null".
  const ops = ALL_FILTER_OPS.slice().sort(
    (a, b) => b.displayName.length - a.displayName.length,
  );

  const opRegex = ops
    .map((op) => op.displayName.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'))
    .join('|');

  // A regex to capture the column, operator and value.
  // The value can be a quoted string or a single word.
  const regex = new RegExp(
    `^(\\S+)\\s+(${opRegex})(?:\\s+(".*?"|'.*?'|\\S+))?$`,
    'i',
  );
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

  const result: Partial<UIFilter> = {
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

  // If the value is quoted, remove the quotes.
  if (
    (value.startsWith('"') && value.endsWith('"')) ||
    (value.startsWith("'") && value.endsWith("'"))
  ) {
    return value.slice(1, -1);
  }

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

export function createFiltersProto(
  filters: UIFilter[] | undefined,
  sourceCols: ColumnInfo[],
): protos.PerfettoSqlStructuredQuery.Filter[] | undefined {
  if (filters === undefined || filters.length === 0) {
    return undefined;
  }

  const protoFilters: protos.PerfettoSqlStructuredQuery.Filter[] = filters.map(
    (f: UIFilter): protos.PerfettoSqlStructuredQuery.Filter => {
      const result = new protos.PerfettoSqlStructuredQuery.Filter();
      result.columnName = f.column;

      const op = ALL_FILTER_OPS.find((o) => o.displayName === f.op);
      if (op === undefined) {
        // Should be handled by validation before this.
        throw new Error(`Unknown filter operator: ${f.op}`);
      }
      result.op = op.proto;

      if ('value' in f) {
        const value = f.value;
        const col = sourceCols.find((c) => c.name === f.column);
        if (typeof value === 'string') {
          result.stringRhs = [value];
        } else if (typeof value === 'number' || typeof value === 'bigint') {
          if (col && (col.type === 'long' || col.type === 'int')) {
            result.int64Rhs = [Number(value)];
          } else {
            result.doubleRhs = [Number(value)];
          }
        }
        // Not handling Uint8Array here. The original FilterToProto also didn't seem to.
      }
      return result;
    },
  );
  return protoFilters;
}
