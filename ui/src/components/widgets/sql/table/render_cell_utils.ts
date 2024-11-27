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
import {TableManager, SqlColumn} from './column';
import {MenuItem, PopupMenu2} from '../../../../widgets/menu';
import {SqlValue} from '../../../../trace_processor/query_result';
import {isString} from '../../../../base/object_utils';
import {sqliteString} from '../../../../base/string_utils';
import {Icons} from '../../../../base/semantic_icons';
import {copyToClipboard} from '../../../../base/clipboard';
import {sqlValueToReadableString} from '../../../../trace_processor/sql_utils';
import {Anchor} from '../../../../widgets/anchor';

interface FilterOp {
  op: string;
  requiresParam: boolean; // Denotes if the operator acts on an input value
}

export enum FilterOption {
  GLOB = 'glob',
  EQUALS_TO = 'equals to',
  NOT_EQUALS_TO = 'not equals to',
  GREATER_THAN = 'greater than',
  GREATER_OR_EQUALS_THAN = 'greater or equals than',
  LESS_THAN = 'less than',
  LESS_OR_EQUALS_THAN = 'less or equals than',
  IS_NULL = 'is null',
  IS_NOT_NULL = 'is not null',
}

export const FILTER_OPTION_TO_OP: Record<FilterOption, FilterOp> = {
  [FilterOption.GLOB]: {op: 'glob', requiresParam: true},
  [FilterOption.EQUALS_TO]: {op: '=', requiresParam: true},
  [FilterOption.NOT_EQUALS_TO]: {op: '!=', requiresParam: true},
  [FilterOption.GREATER_THAN]: {op: '>', requiresParam: true},
  [FilterOption.GREATER_OR_EQUALS_THAN]: {op: '>=', requiresParam: true},
  [FilterOption.LESS_THAN]: {op: '<', requiresParam: true},
  [FilterOption.LESS_OR_EQUALS_THAN]: {op: '<=', requiresParam: true},
  [FilterOption.IS_NULL]: {op: 'IS NULL', requiresParam: false},
  [FilterOption.IS_NOT_NULL]: {op: 'IS NOT NULL', requiresParam: false},
};

export const NUMERIC_FILTER_OPTIONS = [
  FilterOption.EQUALS_TO,
  FilterOption.NOT_EQUALS_TO,
  FilterOption.GREATER_THAN,
  FilterOption.GREATER_OR_EQUALS_THAN,
  FilterOption.LESS_THAN,
  FilterOption.LESS_OR_EQUALS_THAN,
];

export const STRING_FILTER_OPTIONS = [
  FilterOption.EQUALS_TO,
  FilterOption.NOT_EQUALS_TO,
];

export const NULL_FILTER_OPTIONS = [
  FilterOption.IS_NULL,
  FilterOption.IS_NOT_NULL,
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
      tableManager.addFilter({op: filterOp, columns: [column]});
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
    return NULL_FILTER_OPTIONS.map((option) =>
      filterOptionMenuItem(
        option,
        c,
        (cols) => `${cols[0]} ${FILTER_OPTION_TO_OP[option].op}`,
        tableManager,
      ),
    );
  }
  if (isString(value)) {
    return STRING_FILTER_OPTIONS.map((option) =>
      filterOptionMenuItem(
        option,
        c,
        (cols) =>
          `${cols[0]} ${FILTER_OPTION_TO_OP[option].op} ${sqliteString(value)}`,
        tableManager,
      ),
    );
  }
  if (typeof value === 'bigint' || typeof value === 'number') {
    return NUMERIC_FILTER_OPTIONS.map((option) =>
      filterOptionMenuItem(
        option,
        c,
        (cols) => `${cols[0]} ${FILTER_OPTION_TO_OP[option].op} ${value}`,
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
    return m('i', 'NULL');
  }
  return sqlValueToReadableString(value);
}

export function renderStandardCell(
  value: SqlValue,
  column: SqlColumn,
  tableManager: TableManager,
): m.Children {
  const contextMenuItems: m.Child[] = getStandardContextMenuItems(
    value,
    column,
    tableManager,
  );
  return m(
    PopupMenu2,
    {
      trigger: m(Anchor, displayValue(value)),
    },
    ...contextMenuItems,
  );
}
