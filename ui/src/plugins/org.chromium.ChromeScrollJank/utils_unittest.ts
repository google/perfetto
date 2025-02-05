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

import protos from '../../protos';
import {
  createQueryResult,
  LONG_NULL,
  NUM,
  STR,
} from '../../trace_processor/query_result';
import {fromSqlBool, rows} from './utils';

const T = protos.QueryResult.CellsBatch.CellType;

test('rows', () => {
  // Manually construct a QueryResult which is equivalent to running the
  // following SQL query:
  //
  // SELECT
  //   column1,
  //   column2,
  //   column3
  // FROM (
  //   VALUES
  //   ('A', 10, 100),
  //   ('B', 20, 200),
  //   ('C', 30, NULL)
  // )
  // ORDER BY column1 ASC
  const batch = protos.QueryResult.CellsBatch.create({
    cells: [
      [T.CELL_STRING, T.CELL_FLOAT64, T.CELL_VARINT],
      [T.CELL_STRING, T.CELL_FLOAT64, T.CELL_VARINT],
      [T.CELL_STRING, T.CELL_FLOAT64, T.CELL_NULL],
    ].flat(),
    stringCells: ['A', 'B', 'C'].join('\0'),
    float64Cells: [10, 20, 30],
    varintCells: [100, 200],
    isLastBatch: true,
  });
  const resultProto = protos.QueryResult.create({
    columnNames: ['column1', 'column2', 'column3'],
    batch: [batch],
  });
  const queryResult = createQueryResult({query: 'Some query'});
  queryResult.appendResultBatch(
    protos.QueryResult.encode(resultProto).finish(),
  );

  expect(
    rows(queryResult, {column1: STR, column2: NUM, column3: LONG_NULL}),
  ).toStrictEqual([
    {column1: 'A', column2: 10, column3: 100n},
    {column1: 'B', column2: 20, column3: 200n},
    {column1: 'C', column2: 30, column3: null},
  ]);
});

test('fromSqlBool', () => {
  expect(fromSqlBool(null)).toBeUndefined();
  expect(fromSqlBool(-1)).toStrictEqual(true);
  expect(fromSqlBool(0)).toStrictEqual(false);
  expect(fromSqlBool(0.1)).toStrictEqual(true);
  expect(fromSqlBool(1)).toStrictEqual(true);
});
