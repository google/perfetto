// Copyright (C) 2026 The Android Open Source Project
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

import protos from '../../protos';
import type {Trace} from '../../public/trace';
import {createQueryResult} from '../../trace_processor/query_result';
import {loadSqlModulesFromTp} from './sql_modules_from_tp';
import type {StdlibModuleImpl} from './sql_modules_impl';

const CELL_TYPE = protos.QueryResult.CellsBatch.CellType;

type ObjectRow = readonly [
  packageName: string,
  module: string,
  name: string,
  objectType: string,
  description: string,
  exposed: number,
  returnType: string,
  returnDescription: string,
  args: string,
  columns: string,
  // JSON array of included module names. Defaults to '[]' when omitted.
  includes?: string,
];

function makeQueryResult(rows: readonly ObjectRow[]) {
  const strings = rows.flatMap((row) => [
    row[0],
    row[1],
    row[2],
    row[3],
    row[4],
    row[6],
    row[7],
    row[8],
    row[9],
    row[10] ?? '[]',
  ]);
  const cells = rows.flatMap(() => [
    CELL_TYPE.CELL_STRING,
    CELL_TYPE.CELL_STRING,
    CELL_TYPE.CELL_STRING,
    CELL_TYPE.CELL_STRING,
    CELL_TYPE.CELL_STRING,
    CELL_TYPE.CELL_VARINT,
    CELL_TYPE.CELL_STRING,
    CELL_TYPE.CELL_STRING,
    CELL_TYPE.CELL_STRING,
    CELL_TYPE.CELL_STRING,
    CELL_TYPE.CELL_STRING,
  ]);
  const batch = protos.QueryResult.CellsBatch.create({
    cells,
    stringCells: strings.join('\0'),
    varintCells: rows.map((row) => row[5]),
    isLastBatch: true,
  });
  const resultProto = protos.QueryResult.create({
    columnNames: [
      'package',
      'module',
      'name',
      'object_type',
      'description',
      'exposed',
      'return_type',
      'return_description',
      'args',
      'cols',
      'includes',
    ],
    batch: [batch],
  });
  const result = createQueryResult({query: 'stdlib objects'});
  result.appendResultBatch(protos.QueryResult.encode(resultProto).finish());
  return result;
}

test('loads the unified stdlib object catalog', async () => {
  const entry = (name: string, type: string) =>
    JSON.stringify([{name, type, description: `${name} description`}]);
  const result = makeQueryResult([
    ['test', 'test.module', 'test.module', 'MODULE', '', 1, '', '', '[]', '[]'],
    [
      'test',
      'test.module',
      'test_table',
      'TABLE',
      'A test table.',
      1,
      '',
      '',
      '[]',
      entry('value', 'LONG'),
    ],
    [
      'test',
      'test.module',
      'test_function',
      'FUNCTION',
      'A test function.',
      1,
      'LONG',
      'The result.',
      entry('input', 'LONG'),
      '[]',
    ],
    [
      'test',
      'test.module',
      'test_table_function',
      'TABLE_FUNCTION',
      'A test table function.',
      1,
      'TABLE',
      '',
      entry('input', 'LONG'),
      entry('output', 'STRING'),
    ],
    [
      'test',
      'test.module',
      'test_macro',
      'MACRO',
      'A test macro.',
      1,
      'Expr',
      'The expression.',
      entry('input', 'Expr'),
      '[]',
    ],
    [
      'test',
      'test.module',
      '_internal_table',
      'TABLE',
      'Internal.',
      0,
      '',
      '',
      '[]',
      '[]',
    ],
  ]);
  const queries: string[] = [];
  const trace = {
    engine: {
      query: async (sql: string) => {
        queries.push(sql);
        return result;
      },
    },
  } as unknown as Trace;

  const sqlModules = await loadSqlModulesFromTp(trace, {
    'test.module': {
      tags: ['test'],
      data_check_sql: null,
      tables: {
        test_table: {importance: 'high', data_check_sql: null},
      },
    },
  });

  expect(queries).toHaveLength(1);
  expect(queries[0]).toContain('FROM __intrinsic_stdlib_objects');
  const module = sqlModules.listModules()[0]!;
  expect(module.includeKey).toBe('test.module');
  expect(module.tables.map((table) => table.name)).toEqual(['test_table']);
  expect(module.tables[0]!.importance).toBe('high');
  expect(module.tables[0]!.columns[0]!.name).toBe('value');
  expect(module.functions[0]!.name).toBe('test_function');
  expect(module.functions[0]!.returnType).toBe('LONG');
  expect(module.tableFunctions[0]!.name).toBe('test_table_function');
  expect(module.tableFunctions[0]!.returnCols[0]!.name).toBe('output');
  expect(module.macros[0]!.name).toBe('test_macro');
});

test('takes module includes from TP, not from the build-time metadata', async () => {
  const result = makeQueryResult([
    [
      'test',
      'test.with_includes',
      'test.with_includes',
      'MODULE',
      '',
      1,
      '',
      '',
      '[]',
      '[]',
      JSON.stringify(['test.a', 'test.b']),
    ],
    [
      'test',
      'test.no_includes',
      'test.no_includes',
      'MODULE',
      '',
      1,
      '',
      '',
      '[]',
      '[]',
    ],
    // A module the build-time metadata knows nothing about (e.g. one from an
    // extension package or --override-stdlib).
    [
      'ext',
      'ext.module',
      'ext.module',
      'MODULE',
      '',
      1,
      '',
      '',
      '[]',
      '[]',
      JSON.stringify(['test.a']),
    ],
  ]);
  const trace = {
    engine: {query: async () => result},
  } as unknown as Trace;

  const sqlModules = await loadSqlModulesFromTp(trace, {
    // Stale includes in the JSON must be ignored.
    'test.with_includes': {tags: [], includes: ['stale.module']},
  } as unknown as Parameters<typeof loadSqlModulesFromTp>[1]);

  const byKey = new Map(
    sqlModules
      .listModules()
      .map((m) => [m.includeKey, m as StdlibModuleImpl] as const),
  );
  expect(byKey.get('test.with_includes')!.includes).toEqual([
    'test.a',
    'test.b',
  ]);
  expect(byKey.get('test.no_includes')!.includes).toEqual([]);
  expect(byKey.get('ext.module')!.includes).toEqual(['test.a']);
});
