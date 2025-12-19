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
import {Chip} from '../../../../widgets/chip';
import {Intent} from '../../../../widgets/common';
import {SqlValue} from '../../../../trace_processor/query_result';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import {Filter} from '../../../../components/widgets/datagrid/model';

// ============================================================================
// Filter Type Definitions
// ============================================================================

interface FilterValue {
  readonly column: string;
  readonly op: '=' | '!=' | '<' | '<=' | '>' | '>=' | 'glob';
  readonly value: SqlValue;
  enabled?: boolean; // Default true - controls if filter is active
}

interface FilterIn {
  readonly column: string;
  readonly op: 'in' | 'not in';
  readonly value: ReadonlyArray<SqlValue>;
  enabled?: boolean; // Default true - controls if filter is active
}

interface FilterNull {
  readonly column: string;
  readonly op: 'is null' | 'is not null';
  enabled?: boolean; // Default true - controls if filter is active
}

export type UIFilter = FilterValue | FilterNull | FilterIn;

// ============================================================================
// Shared Filter Formatting Utilities
// ============================================================================

/**
 * Maximum number of filter values to display inline before showing a count.
 * For arrays with more values than this, we show "(N values)" instead of listing them.
 * This threshold balances readability with information density - showing too many values
 * clutters the UI, while showing too few loses helpful context.
 */
const MAX_DISPLAY_VALUES = 3;

/**
 * Formats a filter's value portion for display.
 * Handles single values, array values, and null operators.
 *
 * @param filter The filter to format
 * @param includeColumn Whether to include the column name (default: false)
 * @returns Formatted string representation
 */
export function formatFilterValue(
  filter: UIFilter,
  includeColumn = false,
): string {
  const prefix = includeColumn ? `${filter.column} ` : '';

  if ('value' in filter) {
    if (Array.isArray(filter.value)) {
      // Format array of values
      if (filter.value.length > MAX_DISPLAY_VALUES) {
        return `${prefix}${filter.op} (${filter.value.length} values)`;
      } else {
        const formattedValues = filter.value
          .map((v) => (typeof v === 'string' ? `"${v}"` : String(v)))
          .join(', ');
        return `${prefix}${filter.op} (${formattedValues})`;
      }
    }
    // Single value
    const valueStr =
      typeof filter.value === 'string'
        ? `"${filter.value}"`
        : String(filter.value);
    return `${prefix}${filter.op} ${valueStr}`;
  }
  // Null operators (is null, is not null)
  return `${prefix}${filter.op}`;
}

/**
 * Converts a DataGridFilter to an array of UIFilters.
 * FilterIn types (IN/NOT IN) are expanded into multiple equality filters
 * since the query builder doesn't support native IN operations yet.
 *
 * @param filter The filter from the DataGrid to normalize
 * @returns Array of UIFilters (single filter unless IN/NOT IN)
 */
export function normalizeDataGridFilter(filter: Filter): UIFilter[] {
  // Handle IN/NOT IN filters by converting to multiple equality filters
  if (filter.op === 'in' || filter.op === 'not in') {
    const values = filter.value as ReadonlyArray<SqlValue>;

    // Reject empty arrays - this indicates a programming error
    if (values.length === 0) {
      throw new Error(
        `Cannot add ${filter.op} filter with empty values for column "${filter.field}". ` +
          `This likely indicates a bug in the filter selection UI.`,
      );
    }

    const equalityOp = filter.op === 'in' ? '=' : '!=';

    return values.map((value) => ({
      column: filter.field,
      op: equalityOp,
      value: value,
    }));
  }

  // Null filters (is null / is not null)
  if (filter.op === 'is null' || filter.op === 'is not null') {
    return [{column: filter.field, op: filter.op}];
  }

  // Value filters - map 'not glob' to 'glob' (UIFilter doesn't have 'not glob')
  // Note: this loses the negation, but preserves the pattern matching behavior
  // After excluding null filters and in/not in, we know filter has a single value
  const valueFilter = filter as {
    readonly op: '=' | '!=' | '<' | '<=' | '>' | '>=' | 'glob' | 'not glob';
    readonly field: string;
    readonly value: SqlValue;
  };
  const mappedOp =
    valueFilter.op === 'not glob'
      ? ('glob' as const)
      : (valueFilter.op as FilterValue['op']);
  return [{column: valueFilter.field, op: mappedOp, value: valueFilter.value}];
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
    // Also reject empty string values
    if (typeof filter.value === 'string' && filter.value.trim() === '') {
      return false;
    }
  }

  return true;
}

/**
 * Parses a filter from a text string like "dur > 1000" or "name glob '*render*'".
 * Returns a Partial<UIFilter> that can be validated with isFilterDefinitionValid.
 *
 * This is a best-effort parser for simple filters. Supports:
 * - Comparison operators: =, !=, <, <=, >, >=
 * - Null operators: is null, is not null
 * - Pattern matching: glob
 * - Quoted string values: "value" or 'value'
 */
export function parseFilterFromText(
  text: string,
  sourceCols: ColumnInfo[],
): Partial<UIFilter> {
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

export function isValueRequired(op?: FilterOp): boolean {
  return op !== undefined && op.key !== 'IS_NULL' && op.key !== 'IS_NOT_NULL';
}

// Parses a comma-separated string of values into an array of strings or
// numbers.
// If all values can be parsed as numbers, it returns a number array.
// Otherwise, it returns a string array.
export function parseFilterValue(text: string): SqlValue | undefined {
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

// ============================================================================
// Proto Generation
// ============================================================================

/**
 * Validates that all values in an array are of the same type.
 * Mixed-type arrays are not supported in the proto format.
 *
 * @param values Array of values to validate
 * @returns The detected type ('string', 'number', or 'mixed'), or 'empty' for empty arrays
 */
function detectValueType(
  values: ReadonlyArray<SqlValue>,
): 'string' | 'number' | 'mixed' | 'empty' {
  if (values.length === 0) return 'empty';

  const hasString = values.some((v) => typeof v === 'string');
  const hasNumber = values.some(
    (v) => typeof v === 'number' || typeof v === 'bigint',
  );

  if (hasString && hasNumber) {
    return 'mixed';
  } else if (hasString) {
    return 'string';
  } else {
    return 'number';
  }
}

/**
 * Partitions an array of SqlValues by type for proto generation.
 * Numbers are further split into int64 vs double based on column type.
 *
 * @param values Array of values to partition
 * @param column Optional column info for determining int64 vs double
 * @returns Object with stringValues, int64Values, and doubleValues arrays
 */
function partitionValuesByType(
  values: ReadonlyArray<SqlValue>,
  column: ColumnInfo | undefined,
): {
  stringValues: string[];
  int64Values: number[];
  doubleValues: number[];
} {
  const stringValues: string[] = [];
  const int64Values: number[] = [];
  const doubleValues: number[] = [];

  for (const value of values) {
    if (typeof value === 'string') {
      stringValues.push(value);
    } else if (typeof value === 'number' || typeof value === 'bigint') {
      if (column && (column.type === 'long' || column.type === 'int')) {
        int64Values.push(Number(value));
      } else {
        doubleValues.push(Number(value));
      }
    }
  }

  return {stringValues, int64Values, doubleValues};
}

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

        // Handle 'in' and 'not in' operators specially
        // Note: The proto uses EQUAL/NOT_EQUAL with array RHS values to represent
        // OR-ed equality checks. This is because the query builder doesn't have
        // native IN operator support yet.
        if (f.op === 'in' || f.op === 'not in') {
          // Map 'in' to EQUAL with multiple values, 'not in' to NOT_EQUAL
          result.op =
            f.op === 'in'
              ? protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL
              : protos.PerfettoSqlStructuredQuery.Filter.Operator.NOT_EQUAL;

          // Handle array of values
          const values = f.value;
          const col = sourceCols.find((c) => c.name === f.column);

          // Validate that all values are the same type - fail fast on mixed types
          const valueType = detectValueType(values);
          if (valueType === 'mixed') {
            throw new Error(
              `Filter on column "${f.column}" has mixed-type values (strings and numbers). ` +
                `All values must be of the same type. Values: ${JSON.stringify(values)}`,
            );
          }

          // Separate values by type using helper
          const {stringValues, int64Values, doubleValues} =
            partitionValuesByType(values, col);

          // Set the appropriate rhs field based on predominant type
          // Priority: strings > int64 > double
          if (stringValues.length > 0) {
            result.stringRhs = stringValues;
          } else if (int64Values.length > 0) {
            result.int64Rhs = int64Values;
          } else if (doubleValues.length > 0) {
            result.doubleRhs = doubleValues;
          }

          return result;
        }

        // Handle other operators
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
 * Helper to format a single filter as a readable string.
 */
function formatSingleFilter(filter: UIFilter): string {
  return formatFilterValue(filter, true);
}

/**
 * Helper to create a toggle callback for filter enable/disable in nodeDetails.
 */
function createFilterToggleCallback(state: {
  filters?: Partial<UIFilter>[];
  onchange?: () => void;
}): (filter: UIFilter) => void {
  return (filter: UIFilter) => {
    if (state.filters) {
      state.filters = [...state.filters].map((f) =>
        f === filter ? {...f, enabled: f.enabled !== false ? false : true} : f,
      );
      state.onchange?.();
      m.redraw();
    }
  };
}

/**
 * Helper to format filter details for nodeDetails display.
 * Returns interactive chips that can be clicked to toggle enabled/disabled state.
 *
 * Pass the node state to enable interactive toggling, or omit for read-only display.
 */
export function formatFilterDetails(
  filters: UIFilter[] | undefined,
  filterOperator: 'AND' | 'OR' | undefined,
  state?: {filters?: Partial<UIFilter>[]; onchange?: () => void},
  onRemove?: (filter: UIFilter) => void,
  compact?: boolean,
  onEdit?: (filter: UIFilter) => void,
): m.Child | undefined {
  if (!filters || filters.length === 0) {
    return undefined;
  }

  // Create default onRemove handler if state is provided but onRemove is not
  const effectiveOnRemove =
    onRemove ??
    (state
      ? (filter: UIFilter) => {
          state.filters = (state.filters ?? []).filter((f) => f !== filter);
          state.onchange?.();
        }
      : undefined);

  const count = filters.length;
  const enabledCount = filters.filter((f) => f.enabled !== false).length;
  const operator = filterOperator ?? 'AND';
  const onFilterToggle = state ? createFilterToggleCallback(state) : undefined;

  // Helper to render a filter chip
  const renderFilterChip = (filter: UIFilter) => {
    const isEnabled = filter.enabled !== false;
    const label = formatSingleFilter(filter);
    const classNames = [
      'pf-filter-chip-wrapper',
      !isEnabled && 'pf-filter-chip-wrapper--disabled',
      compact && 'pf-filter-chip-wrapper--compact',
    ]
      .filter(Boolean)
      .join(' ');

    return m(
      'span',
      {
        className: classNames,
        style: {
          display: 'inline-flex',
          alignItems: 'center',
          gap: '4px',
        },
      },
      m(Chip, {
        label,
        rounded: true,
        removable: !!effectiveOnRemove,
        intent: isEnabled ? Intent.Primary : Intent.None,
        onclick: onFilterToggle
          ? (e: MouseEvent) => {
              e.preventDefault();
              e.stopPropagation();
              onFilterToggle(filter);
            }
          : undefined,
        onRemove: effectiveOnRemove
          ? () => effectiveOnRemove(filter)
          : undefined,
        style: {cursor: 'pointer'},
        oncontextmenu: onEdit
          ? (e: MouseEvent) => {
              e.preventDefault();
              e.stopPropagation();
              onEdit(filter);
            }
          : undefined,
      }),
    );
  };

  // For 4 or fewer filters
  if (count <= 4) {
    const filterChipsClass = compact
      ? '.pf-filter-chips.pf-filter-chips--compact'
      : '.pf-filter-chips';

    // Show operator badge only when there are 2+ filters
    const showOperatorBadge = count >= 2;

    return m(
      '.pf-filter-container',
      showOperatorBadge &&
        m(
          '.pf-filter-operator-header',
          m('.pf-filter-operator-badge', operator),
        ),
      m(
        filterChipsClass,
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
