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

import {errResult, okResult} from '../base/result';
import {
  parsePerfettoSqlTypeFromString,
  perfettoSqlTypeToString,
} from './perfetto_sql_type';

test('PerfettoSqlType.ParseSimpleTypes', () => {
  const TEST_CASES: Record<string, string> = {
    int: 'int',
    long: 'int',
    bool: 'boolean',
    float: 'double',
    double: 'double',
    string: 'string',
    bytes: 'bytes',
    timestamp: 'timestamp',
    duration: 'duration',
    argsetid: 'arg_set_id',
  };

  for (const [rawInput, expectedKind] of Object.entries(TEST_CASES)) {
    for (const input of [rawInput, rawInput.toUpperCase()]) {
      expect(parsePerfettoSqlTypeFromString(input)).toEqual(
        okResult({kind: expectedKind}),
      );
    }
  }
});

test('PerfettoSqlType.ParseIdTypes', () => {
  expect(parsePerfettoSqlTypeFromString('id')).toEqual(
    okResult({
      kind: 'id',
    }),
  );
  expect(parsePerfettoSqlTypeFromString('ID')).toEqual(
    okResult({
      kind: 'id',
    }),
  );
  expect(parsePerfettoSqlTypeFromString('id(slice.id)')).toEqual(
    okResult({
      kind: 'id',
      source: {
        table: 'slice',
        column: 'id',
      },
    }),
  );
  expect(parsePerfettoSqlTypeFromString('ID(thread.utid)')).toEqual(
    okResult({
      kind: 'id',
      source: {
        table: 'thread',
        column: 'utid',
      },
    }),
  );
  expect(parsePerfettoSqlTypeFromString('Id(counter.id)')).toEqual(
    okResult({
      kind: 'id',
      source: {
        table: 'counter',
        column: 'id',
      },
    }),
  );
  expect(parsePerfettoSqlTypeFromString('joinid(slice.id)')).toEqual(
    okResult({
      kind: 'joinid',
      source: {
        table: 'slice',
        column: 'id',
      },
    }),
  );
  expect(parsePerfettoSqlTypeFromString('JOINID(thread.utid)')).toEqual(
    okResult({
      kind: 'joinid',
      source: {
        table: 'thread',
        column: 'utid',
      },
    }),
  );
  expect(parsePerfettoSqlTypeFromString('JoinId(process_table.upid)')).toEqual(
    okResult({
      kind: 'joinid',
      source: {
        table: 'process_table',
        column: 'upid',
      },
    }),
  );
});

test('PerfettoSqlType.ParseUnknownTypes', () => {
  expect(parsePerfettoSqlTypeFromString('unknown')).toEqual(
    errResult('Unknown type: unknown'),
  );

  expect(parsePerfettoSqlTypeFromString('notavalidtype')).toEqual(
    errResult('Unknown type: notavalidtype'),
  );

  expect(parsePerfettoSqlTypeFromString('')).toEqual(
    errResult('Unknown type: '),
  );
});

test('PerfettoSqlType.ToString', () => {
  expect(perfettoSqlTypeToString({kind: 'int'})).toBe('INT');
  expect(perfettoSqlTypeToString({kind: 'double'})).toBe('DOUBLE');
  expect(perfettoSqlTypeToString({kind: 'string'})).toBe('STRING');
  expect(perfettoSqlTypeToString({kind: 'boolean'})).toBe('BOOLEAN');
  expect(perfettoSqlTypeToString({kind: 'bytes'})).toBe('BYTES');
  expect(perfettoSqlTypeToString({kind: 'timestamp'})).toBe('TIMESTAMP');
  expect(perfettoSqlTypeToString({kind: 'duration'})).toBe('DURATION');
  expect(perfettoSqlTypeToString({kind: 'arg_set_id'})).toBe('ARG_SET_ID');
  expect(perfettoSqlTypeToString({kind: 'id'})).toBe('ID');
  expect(
    perfettoSqlTypeToString({
      kind: 'id',
      source: {
        table: 'slice',
        column: 'id',
      },
    }),
  ).toBe('ID(slice.id)');
  expect(
    perfettoSqlTypeToString({
      kind: 'joinid',
      source: {
        table: 'thread',
        column: 'utid',
      },
    }),
  ).toBe('JOINID(thread.utid)');
});
