// Copyright (C) 2024 The Android Open Source Project
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
import {SqlColumn} from './sql_column';
import {MenuItem} from '../../../../widgets/menu';
import {SqlValue} from '../../../../trace_processor/query_result';
import {isString} from '../../../../base/object_utils';
import {sqliteString} from '../../../../base/string_utils';
import {Icons} from '../../../../base/semantic_icons';
import {copyToClipboard} from '../../../../base/clipboard';
import {sqlValueToReadableString} from '../../../../trace_processor/sql_utils';
import {RenderedCell, TableManager} from './table_column';

export interface LegacySqlTableFilterOp {
  op: string; // string representation of the operation (to be injected to SQL)
  label: LegacySqlTableFilterLabel; // human readable name for operation
  requiresParam?: boolean; // Denotes if the operator acts on an input value
}

export type LegacySqlTableFilterLabel =
  | 'glob'
  | 'equals to'
  | 'not equals to'
  | 'greater than'
  | 'greater or equals than'
  | 'less than'
  | 'less or equals than'
  | 'is null'
  | 'is not null';

export const LegacySqlTableFilterOptions: Record<
  LegacySqlTableFilterLabel,
  LegacySqlTableFilterOp
> = {
  'glob': {op: 'glob', label: 'glob', requiresParam: true},
  'equals to': {op: '=', label: 'equals to', requiresParam: true},
  'not equals to': {op: '!=', label: 'not equals to', requiresParam: true},
  'greater than': {op: '>', label: 'greater than', requiresParam: true},
  'greater or equals than': {
    op: '>=',
    label: 'greater or equals than',
    requiresParam: true,
  },
  'less than': {op: '<', label: 'less than', requiresParam: true},
  'less or equals than': {
    op: '<=',
    label: 'less or equals than',
    requiresParam: true,
  },
  'is null': {op: 'IS NULL', label: 'is null', requiresParam: false},
  'is not null': {
    op: 'IS NOT NULL',
    label: 'is not null',
    requiresParam: false,
  },
};

export const NUMERIC_FILTER_OPTIONS: LegacySqlTableFilterLabel[] = [
  'equals to',
  'not equals to',
  'greater than',
  'greater or equals than',
  'less than',
  'less or equals than',
];

export const STRING_FILTER_OPTIONS: LegacySqlTableFilterLabel[] = [
  'equals to',
  'not equals to',
];

export const NULL_FILTER_OPTIONS: LegacySqlTableFilterLabel[] = [
  'is null',
  'is not null',
];

function filterOptionMenuItem(
  label: string,
  column: SqlColumn,
  filterOp: (cols: string[]) => string,
  tableManager: TableManager,
): m.Child {
  return m(MenuItem, {
    label,
    onclick: () => {
      tableManager.filters.addFilter({op: filterOp, columns: [column]});
    },
  });
}

// Return a list of "standard" menu items, adding corresponding filters to the given cell.
export function getStandardFilters(
  value: SqlValue,
  c: SqlColumn,
  tableManager: TableManager,
): m.Child[] {
  if (value === null) {
    return NULL_FILTER_OPTIONS.map((label) =>
      filterOptionMenuItem(
        label,
        c,
        (cols) => `${cols[0]} ${LegacySqlTableFilterOptions[label].op}`,
        tableManager,
      ),
    );
  }
  if (isString(value)) {
    return STRING_FILTER_OPTIONS.map((label) =>
      filterOptionMenuItem(
        label,
        c,
        (cols) =>
          `${cols[0]} ${LegacySqlTableFilterOptions[label].op} ${sqliteString(value)}`,
        tableManager,
      ),
    );
  }
  if (typeof value === 'bigint' || typeof value === 'number') {
    return NUMERIC_FILTER_OPTIONS.map((label) =>
      filterOptionMenuItem(
        label,
        c,
        (cols) =>
          `${cols[0]} ${LegacySqlTableFilterOptions[label].op} ${value}`,
        tableManager,
      ),
    );
  }
  return [];
}

function copyMenuItem(label: string, value: string): m.Child {
  return m(MenuItem, {
    icon: Icons.Copy,
    label,
    onclick: () => {
      copyToClipboard(value);
    },
  });
}

// Return a list of "standard" menu items for the given cell.
export function getStandardContextMenuItems(
  value: SqlValue,
  column: SqlColumn,
  tableManager: TableManager,
): m.Child[] {
  const result: m.Child[] = [];

  if (isString(value)) {
    result.push(copyMenuItem('Copy', value));
  }

  const filters = getStandardFilters(value, column, tableManager);
  if (filters.length > 0) {
    result.push(
      m(MenuItem, {label: 'Add filter', icon: Icons.Filter}, ...filters),
    );
  }

  return result;
}

export function displayValue(value: SqlValue): m.Child {
  if (value === null) {
    return 'null';
  }
  return sqlValueToReadableString(value);
}

export function renderStandardCell(
  value: SqlValue,
  column: SqlColumn,
  tableManager: TableManager | undefined,
): RenderedCell {
  const contentWithFormatting = {
    content: displayValue(value),
    isNumerical: typeof value === 'number' || typeof value === 'bigint',
    isNull: value == null,
  };

  if (tableManager === undefined) {
    return contentWithFormatting;
  }
  const contextMenuItems: m.Child[] = getStandardContextMenuItems(
    value,
    column,
    tableManager,
  );
  return {
    ...contentWithFormatting,
    menu: contextMenuItems,
  };
}
