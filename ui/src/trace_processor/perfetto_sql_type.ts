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

import {errResult, okResult, Result} from '../base/result';

// Representation of a PerfettoSQL type:
// https://perfetto.dev/docs/analysis/perfetto-sql-syntax#types
export type PerfettoSqlType =
  | SimpleType
  | {
      kind: 'id';
      source?: {
        table: string;
        column: string;
      };
    }
  | {
      kind: 'joinid';
      source: {
        table: string;
        column: string;
      };
    };

type SimpleType = {
  kind:
    | 'int'
    | 'double'
    | 'boolean'
    | 'string'
    | 'bytes'
    | 'timestamp'
    | 'duration'
    | 'arg_set_id';
};

export function isQuantitativeType(type: PerfettoSqlType) {
  switch (type.kind) {
    case 'int':
    case 'double':
    case 'duration':
    case 'timestamp':
    case 'id':
    case 'joinid':
    case 'arg_set_id':
    case 'boolean':
      return true;
    case 'bytes':
    case 'string':
      return false;
  }
}

export class PerfettoSqlTypes {
  static readonly INT: PerfettoSqlType = {kind: 'int'};
  static readonly DOUBLE: PerfettoSqlType = {kind: 'double'};
  static readonly STRING: PerfettoSqlType = {kind: 'string'};
  static readonly BOOLEAN: PerfettoSqlType = {kind: 'boolean'};
  static readonly TIMESTAMP: PerfettoSqlType = {kind: 'timestamp'};
  static readonly DURATION: PerfettoSqlType = {kind: 'duration'};
  static readonly ARG_SET_ID: PerfettoSqlType = {kind: 'arg_set_id'};
}

const SIMPLE_TYPES: Record<string, SimpleType['kind']> = {
  long: 'int',
  int: 'int',
  bool: 'boolean',
  float: 'double',
  double: 'double',
  string: 'string',
  bytes: 'bytes',
  timestamp: 'timestamp',
  duration: 'duration',
  argsetid: 'arg_set_id',
};

export function parsePerfettoSqlTypeFromString(
  typeString: string,
): Result<PerfettoSqlType> {
  const value = typeString.toLowerCase();
  const maybeSimpleType = SIMPLE_TYPES[value];
  if (maybeSimpleType !== undefined) {
    return okResult({
      kind: maybeSimpleType,
    });
  }
  // ID:
  if (value === 'id') {
    return okResult({
      kind: 'id',
    });
  }
  // ID(table.column):
  {
    const match = value.match(/^id\(([^.]+)\.([^)]+)\)$/);
    if (match) {
      return okResult({
        kind: 'id',
        source: {
          table: match[1],
          column: match[2],
        },
      });
    }
  }
  // JOINID(table.column):
  {
    const match = value.match(/^joinid\(([^.]+)\.([^)]+)\)$/);
    if (match) {
      return okResult({
        kind: 'joinid',
        source: {
          table: match[1],
          column: match[2],
        },
      });
    }
  }
  return errResult(`Unknown type: ${typeString}`);
}

export function perfettoSqlTypeToString(type?: PerfettoSqlType): string {
  if (type === undefined) {
    return 'ANY';
  }
  if (type.kind === 'id') {
    if (type.source !== undefined) {
      return `ID(${type.source.table}.${type.source.column})`;
    }
  }
  if (type.kind === 'joinid') {
    return `JOINID(${type.source.table}.${type.source.column})`;
  }
  return type.kind.toUpperCase();
}
