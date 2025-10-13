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

export type PerfettoSqlType =
  | {
      kind:
        | 'int'
        | 'float'
        | 'string'
        | 'boolean'
        | 'bytes'
        | 'timestamp'
        | 'duration'
        | 'arg_set_id';
    }
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

const SIMPLE_TYPES = {
  long: 'int',
  int: 'int',
  bool: 'boolean',
  float: 'float',
  double: 'float',
  string: 'string',
  bytes: 'bytes',
  timestamp: 'timestamp',
  duration: 'duration',
  argsetid: 'arg_set_id',
  id: 'id',
};

export function parsePerfettoSqlTypeFromString(
  typeString: string,
): Result<PerfettoSqlType> {
  const value = typeString.toLowerCase();
  const maybeSimpleType = Object(SIMPLE_TYPES)[value];
  if (maybeSimpleType !== undefined) {
    return okResult({
      kind: maybeSimpleType,
    });
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
