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

import {
  PerfettoSqlType,
  parsePerfettoSqlTypeFromString,
} from '../../../trace_processor/perfetto_sql_type';
import {SqlColumn} from '../../dev.perfetto.SqlModules/sql_modules';

export interface ColumnInfo {
  name: string;
  type?: PerfettoSqlType;
  description?: string;
  checked: boolean;
  alias?: string;
  // When true, the type was explicitly modified by the user and should be
  // preserved even when upstream columns change.
  typeUserModified?: boolean;
}

export function columnInfoFromSqlColumn(
  column: SqlColumn,
  checked: boolean = false,
): ColumnInfo {
  return {
    name: column.name,
    type: column.type,
    description: column.description,
    checked,
  };
}

export function newColumnInfo(
  col: ColumnInfo,
  checked?: boolean | undefined,
): ColumnInfo {
  const finalName = col.alias ?? col.name;
  return {
    name: finalName,
    type: col.type,
    description: col.description,
    alias: undefined,
    checked: checked ?? col.checked,
    typeUserModified: col.typeUserModified,
  };
}

// Handle legacy serialized state where type was a string (e.g. "INT")
// instead of a PerfettoSqlType object (e.g. {kind: 'int'}).
export function legacyDeserializeType(
  type: PerfettoSqlType | string | undefined,
): PerfettoSqlType | undefined {
  if (type === undefined) return undefined;
  if (typeof type === 'string') {
    const parsed = parsePerfettoSqlTypeFromString({type});
    return parsed.ok ? parsed.value : undefined;
  }
  // Already a proper PerfettoSqlType object
  if (type.kind !== undefined) return type;
  return undefined;
}
