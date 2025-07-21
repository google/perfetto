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

import {SqlColumn} from '../../dev.perfetto.SqlModules/sql_modules';

export interface ColumnInfo {
  name: string;
  checked: boolean;
  column: SqlColumn;
  alias?: string;
}

export function columnInfoFromSqlColumn(
  column: SqlColumn,
  checked: boolean = false,
): ColumnInfo {
  return {
    name: column.name,
    checked,
    column: column,
  };
}

export function columnInfoFromName(
  name: string,
  checked: boolean = false,
): ColumnInfo {
  return {
    name,
    checked,
    column: {name, type: {name: 'NA', shortName: 'NA'}},
  };
}

export function newColumnInfo(
  col: ColumnInfo,
  checked?: boolean | undefined,
): ColumnInfo {
  return {
    name: col.alias ?? col.column.name,
    column: col.column,
    alias: undefined,
    checked: checked ?? col.checked,
  };
}

export function newColumnInfoList(
  oldCols: ColumnInfo[],
  checked?: boolean | undefined,
): ColumnInfo[] {
  return oldCols.map((col) => newColumnInfo(col, checked));
}
