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
import {MenuItem, PopupMenu} from '../../../../widgets/menu';
import {
  SIMPLE_TYPE_KINDS,
  isIdType,
  perfettoSqlTypeToString,
} from '../../../../trace_processor/perfetto_sql_type';
import {ColumnInfo} from '../column_info';
import {SqlModules} from '../../../dev.perfetto.SqlModules/sql_modules';

/**
 * Gets list of tables with columns that have a pure ID type
 * Returns array of {tableName, columnName} pairs
 */
function getTablesWithIdColumn(
  sqlModules: SqlModules | undefined,
): Array<{tableName: string; columnName: string}> {
  if (!sqlModules) {
    return [];
  }

  const tablesWithId: Array<{tableName: string; columnName: string}> = [];
  const tables = sqlModules.listTables();

  for (const table of tables) {
    for (const col of table.columns) {
      if (col.type === undefined) continue;
      if (col.type.kind !== 'id') continue;
      if (col.type.source.table !== table.name) continue;
      tablesWithId.push({tableName: table.name, columnName: col.name});
    }
  }
  return tablesWithId.sort((a, b) => a.tableName.localeCompare(b.tableName));
}

/**
 * Renders a type selector for a column
 * @param col The column information
 * @param index The column index
 * @param sqlModules SQL modules for table lookup
 * @param onColumnType Optional callback when type is changed
 * @returns A mithril child with the type selector UI
 */
export function renderTypeSelector(
  col: ColumnInfo,
  index: number,
  sqlModules: SqlModules | undefined,
  onColumnType?: (index: number, type: string) => void,
): m.Child {
  // If no callback provided, don't render type selector
  if (!onColumnType) {
    return null;
  }

  const currentType = col.type ?? 'UNKNOWN';
  const originalType = col.column.type;

  // Build the list of menu items
  const menuItems: m.Child[] = [];

  // If the original type is an ID type, include it as an option
  if (originalType !== undefined && isIdType(originalType)) {
    const idTypeStr = perfettoSqlTypeToString(originalType);
    menuItems.push(
      m(MenuItem, {
        label: idTypeStr,
        active: currentType === idTypeStr,
        onclick: () => onColumnType(index, idTypeStr),
      }),
    );
  }

  // Add all simple types
  for (const type of SIMPLE_TYPE_KINDS) {
    const typeUpper = type.toUpperCase();
    menuItems.push(
      m(MenuItem, {
        label: typeUpper,
        active: currentType === typeUpper,
        onclick: () => onColumnType(index, typeUpper),
      }),
    );
  }

  // Add JOINID submenu with tables that have a pure ID type column
  const tablesWithId = getTablesWithIdColumn(sqlModules);
  const joinidMenuItems =
    tablesWithId.length > 0
      ? tablesWithId.map(({tableName, columnName}) => {
          const joinidType = `JOINID(${tableName}.${columnName})`;
          return m(MenuItem, {
            label: `${tableName}.${columnName}`,
            active: currentType === joinidType,
            onclick: () => onColumnType(index, joinidType),
          });
        })
      : [
          m(MenuItem, {
            label: sqlModules
              ? 'No tables with ID columns'
              : 'Load a trace first',
            disabled: true,
          }),
        ];

  menuItems.push(
    m(
      MenuItem,
      {
        label: 'JOINID...',
      },
      joinidMenuItems,
    ),
  );

  return m(
    PopupMenu,
    {
      trigger: m('.pf-column-type', currentType),
    },
    menuItems,
  );
}
