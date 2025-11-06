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
import {Checkbox} from '../../../../widgets/checkbox';
import {Chip} from '../../../../widgets/chip';
import {Intent} from '../../../../widgets/common';
import {Select} from '../../../../widgets/select';
import {Switch} from '../../../../widgets/switch';
import {TextInput} from '../../../../widgets/text_input';
import {SqlValue} from '../../../../trace_processor/query_result';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import {Stack} from '../../../../widgets/stack';

interface FilterValue {
  readonly column: string;
  readonly op: '=' | '!=' | '<' | '<=' | '>' | '>=' | 'glob';
  readonly value: SqlValue;
  enabled?: boolean; // Default true - controls if filter is active
}

interface FilterNull {
  readonly column: string;
  readonly op: 'is null' | 'is not null';
  enabled?: boolean; // Default true - controls if filter is active
}

export type UIFilter = FilterValue | FilterNull;

/**
 * Attributes for the FilterOperation component.
 */
export interface FilterAttrs {
  readonly sourceCols: ColumnInfo[];
  readonly filters?: ReadonlyArray<UIFilter>;
  readonly filterOperator?: 'AND' | 'OR';
  readonly onFiltersChanged?: (filters: ReadonlyArray<UIFilter>) => void;
  readonly onFilterOperatorChanged?: (operator: 'AND' | 'OR') => void;
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

  private renderFilterChip(
    filter: Partial<UIFilter>,
    attrs: FilterAttrs,
  ): m.Child {
    const isComplete = isFilterDefinitionValid(filter);
    const isEnabled = filter.enabled !== false; // Default to true
    const label = isComplete
      ? `${filter.column} ${filter.op} ${'value' in filter ? filter.value : ''}`
      : 'New Filter';

    return m(
      'div',
      {
        style: {
          display: 'flex',
          alignItems: 'center',
          gap: '4px',
          opacity: isEnabled ? 1 : 0.5,
        },
      },
      isComplete &&
        m(Checkbox, {
          checked: isEnabled,
          onchange: () => {
            const nextFilters = this.uiFilters.map((f) =>
              f === filter ? {...f, enabled: !isEnabled} : f,
            );
            this.setFilters(nextFilters, attrs);
          },
        }),
      m(Chip, {
        label,
        rounded: true,
        intent: isComplete
          ? isEnabled
            ? Intent.Primary
            : Intent.None
          : Intent.None,
        onclick: () => {
          // When we start editing a chip, we remove all other invalid
          // filters from the list.
          const nextFilters = this.uiFilters.filter(
            (f) => f === filter || isFilterDefinitionValid(f),
          );
          this.setFilters(nextFilters, attrs, filter);
        },
      }),
    );
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

    const operator = attrs.filterOperator ?? 'AND';
    const hasMultipleFilters =
      this.uiFilters.filter(isFilterDefinitionValid).length > 1;

    return m(
      '.pf-exp-query-operations',
      m(Card, {}, [
        m(
          '.pf-exp-filters-header',
          m('h2.pf-exp-filters-title', 'Filters'),
          hasMultipleFilters &&
            m(
              '.pf-exp-filter-operator',
              m(Switch, {
                checked: operator === 'OR',
                onchange: () => {
                  const newOp = operator === 'AND' ? 'OR' : 'AND';
                  attrs.onFilterOperatorChanged?.(newOp);
                  attrs.onchange?.();
                },
                labelLeft: 'AND',
                label: 'OR',
                title: 'Toggle between AND and OR operators',
              }),
            ),
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
          {orientation: 'vertical'},
          this.uiFilters.map((filter) => this.renderFilterChip(filter, attrs)),
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

  // Filter out disabled filters (enabled defaults to true if not set)
  const enabledFilters = filters.filter((f) => f.enabled !== false);
  if (enabledFilters.length === 0) {
    return undefined;
  }

  const protoFilters: protos.PerfettoSqlStructuredQuery.Filter[] =
    enabledFilters.map(
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

export function createExperimentalFiltersProto(
  filters: UIFilter[] | undefined,
  sourceCols: ColumnInfo[],
  operator?: 'AND' | 'OR',
): protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup | undefined {
  if (filters === undefined || filters.length === 0) {
    return undefined;
  }

  const protoFilters = createFiltersProto(filters, sourceCols);
  if (!protoFilters) {
    return undefined;
  }

  const filterGroup =
    new protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup();

  // Use the provided operator, defaulting to AND for backward compatibility
  const op = operator ?? 'AND';
  filterGroup.op =
    op === 'OR'
      ? protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator.OR
      : protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator.AND;

  filterGroup.filters = protoFilters;

  return filterGroup;
}

/**
 * Helper to render FilterOperation with standard wiring for any node.
 * This avoids repeating the same pattern in every node type.
 */
export function renderFilterOperation(
  filters: UIFilter[] | undefined,
  filterOperator: 'AND' | 'OR' | undefined,
  sourceCols: ColumnInfo[],
  onFiltersChanged: (filters: ReadonlyArray<UIFilter>) => void,
  onFilterOperatorChanged: (operator: 'AND' | 'OR') => void,
): m.Child {
  return m(FilterOperation, {
    filters,
    filterOperator,
    sourceCols,
    onFiltersChanged,
    onFilterOperatorChanged,
  });
}

/**
 * Helper to format a single filter as a readable string.
 */
function formatSingleFilter(filter: UIFilter): string {
  if ('value' in filter) {
    // FilterValue
    const valueStr =
      typeof filter.value === 'string'
        ? `"${filter.value}"`
        : String(filter.value);
    return `${filter.column} ${filter.op} ${valueStr}`;
  } else {
    // FilterNull
    return `${filter.column} ${filter.op}`;
  }
}

/**
 * Helper to create a toggle callback for filter enable/disable in nodeDetails.
 */
function createFilterToggleCallback(state: {
  filters?: UIFilter[];
  onchange?: () => void;
}): (filter: UIFilter) => void {
  return (filter: UIFilter) => {
    if (state.filters) {
      state.filters = state.filters.map((f) =>
        f === filter ? {...f, enabled: f.enabled !== false ? false : true} : f,
      );
      state.onchange?.();
    }
  };
}

/**
 * Helper to format filter details for nodeDetails display.
 * Returns interactive chips that can be clicked to toggle enabled/disabled state.
 * When using OR operator, shows individual filter chips for visibility.
 *
 * Pass the node state to enable interactive toggling, or omit for read-only display.
 */
export function formatFilterDetails(
  filters: UIFilter[] | undefined,
  filterOperator: 'AND' | 'OR' | undefined,
  state?: {filters?: UIFilter[]; onchange?: () => void},
  onRemove?: (filter: UIFilter) => void,
): m.Child | undefined {
  if (!filters || filters.length === 0) {
    return undefined;
  }

  const count = filters.length;
  const enabledCount = filters.filter((f) => f.enabled !== false).length;
  const operator = filterOperator ?? 'AND';
  const onFilterToggle = state ? createFilterToggleCallback(state) : undefined;

  // Helper to render a filter chip
  const renderFilterChip = (filter: UIFilter) => {
    const isEnabled = filter.enabled !== false;
    const label = formatSingleFilter(filter);
    return m(
      'span',
      {
        className: isEnabled
          ? 'pf-filter-chip-wrapper'
          : 'pf-filter-chip-wrapper pf-filter-chip-wrapper--disabled',
      },
      m(Chip, {
        label,
        rounded: true,
        removable: !!onRemove,
        intent: isEnabled ? Intent.Primary : Intent.None,
        onclick: onFilterToggle ? () => onFilterToggle(filter) : undefined,
        onRemove: onRemove ? () => onRemove(filter) : undefined,
      }),
    );
  };

  // For 4 or fewer filters
  if (count <= 4) {
    // For OR filters, show chips in visual boundary without summary text
    if (operator === 'OR') {
      return m(
        '.pf-filter-or-group',
        m('.pf-filter-or-badge', 'OR'),
        m(
          '.pf-filter-chips.pf-filter-chips--no-count',
          filters.map((filter) => renderFilterChip(filter)),
        ),
      );
    }
    // For AND filters, just show chips
    return m(
      '.pf-filter-container',
      m(
        '.pf-filter-chips',
        filters.map((filter) => renderFilterChip(filter)),
      ),
    );
  }

  // For more than 4 filters with AND operator, show summary only
  if (operator === 'AND') {
    return m(
      '.pf-filter-container',
      m(
        '.pf-filter-and-header',
        m('.pf-filter-operator-badge', 'AND'),
        enabledCount === count
          ? `${count} filters`
          : `${enabledCount} of ${count} enabled`,
      ),
    );
  }

  // For more than 4 filters with OR operator, show summary only
  return m(
    '.pf-filter-container',
    m(
      '.pf-filter-and-header',
      m('.pf-filter-operator-badge', 'OR'),
      enabledCount === count
        ? `${count} filters`
        : `${enabledCount} of ${count} enabled`,
    ),
  );
}
