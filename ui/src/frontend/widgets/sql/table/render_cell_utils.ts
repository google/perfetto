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
    return [
      filterOptionMenuItem(
        'is null',
        c,
        (cols) => `${cols[0]} is null`,
        tableManager,
      ),
      filterOptionMenuItem(
        'is not null',
        c,
        (cols) => `${cols[0]} is not null`,
        tableManager,
      ),
    ];
  }
  if (isString(value)) {
    return [
      filterOptionMenuItem(
        'equals to',
        c,
        (cols) => `${cols[0]} = ${sqliteString(value)}`,
        tableManager,
      ),
      filterOptionMenuItem(
        'not equals to',
        c,
        (cols) => `${cols[0]} != ${sqliteString(value)}`,
        tableManager,
      ),
    ];
  }
  if (typeof value === 'bigint' || typeof value === 'number') {
    return [
      filterOptionMenuItem(
        'equals to',
        c,
        (cols) => `${cols[0]} = ${value}`,
        tableManager,
      ),
      filterOptionMenuItem(
        'not equals to',
        c,
        (cols) => `${cols[0]} != ${value}`,
        tableManager,
      ),
      filterOptionMenuItem(
        'greater than',
        c,
        (cols) => `${cols[0]} > ${value}`,
        tableManager,
      ),
      filterOptionMenuItem(
        'greater or equals than',
        c,
        (cols) => `${cols[0]} >= ${value}`,
        tableManager,
      ),
      filterOptionMenuItem(
        'less than',
        c,
        (cols) => `${cols[0]} < ${value}`,
        tableManager,
      ),
      filterOptionMenuItem(
        'less or equals than',
        c,
        (cols) => `${cols[0]} <= ${value}`,
        tableManager,
      ),
    ];
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

function displayValue(value: SqlValue): m.Child {
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
