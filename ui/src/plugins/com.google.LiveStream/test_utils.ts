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
import {
  createQueryResult,
  type QueryResult,
} from '../../trace_processor/query_result';

/**
 * A query result with `columns` and `rows`, for tests: strings, integers as
 * varints and other numbers as doubles.
 */
export function queryResult(
  columns: string[],
  rows: ReadonlyArray<ReadonlyArray<number | string>> = [],
): QueryResult {
  const cells = rows.flat();
  const numbers = cells.filter((c) => typeof c === 'number');
  const batch = protos.QueryResult.CellsBatch.create({
    cells: cells.map(cellType),
    varintCells: numbers.filter((c) => Number.isInteger(c)),
    float64Cells: numbers.filter((c) => !Number.isInteger(c)),
    stringCells: cells
      .filter((c) => typeof c === 'string')
      .map((c) => `${c}\0`)
      .join(''),
    isLastBatch: true,
  });
  const result = createQueryResult({query: ''});
  result.appendResultBatch(
    protos.QueryResult.encode(
      protos.QueryResult.create({columnNames: columns, batch: [batch]}),
    ).finish(),
  );
  return result;
}

function cellType(cell: number | string) {
  const CellType = protos.QueryResult.CellsBatch.CellType;
  if (typeof cell === 'string') return CellType.CELL_STRING;
  return Number.isInteger(cell) ? CellType.CELL_VARINT : CellType.CELL_FLOAT64;
}
