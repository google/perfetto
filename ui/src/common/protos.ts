// Copyright (C) 2018 The Android Open Source Project
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

import * as protos from '../gen/protos';

// Aliases protos to avoid the super nested namespaces.
// See https://www.typescriptlang.org/docs/handbook/namespaces.html#aliases
import TraceConfig = protos.perfetto.protos.TraceConfig;
import TraceProcessor = protos.perfetto.protos.TraceProcessor;
import IRawQueryArgs = protos.perfetto.protos.IRawQueryArgs;
import RawQueryArgs = protos.perfetto.protos.RawQueryArgs;
import RawQueryResult = protos.perfetto.protos.RawQueryResult;

// TODO(hjd): Maybe these should go in their own file.
export interface Row { [key: string]: number|string; }

function getCell(result: RawQueryResult, column: number, row: number): number|
    string {
  const values = result.columns[column];
  switch (result.columnDescriptors[column].type) {
    case RawQueryResult.ColumnDesc.Type.LONG:
      return +values.longValues![row];
    case RawQueryResult.ColumnDesc.Type.DOUBLE:
      return +values.doubleValues![row];
    case RawQueryResult.ColumnDesc.Type.STRING:
      return values.stringValues![row];
    default:
      throw new Error('Unhandled type!');
  }
}

export function rawQueryResultColumns(result: RawQueryResult): string[] {
  return result.columnDescriptors.map(d => d.name || '');
}

export function* rawQueryResultIter(result: RawQueryResult) {
  const columns: Array<[string, number]> = rawQueryResultColumns(result).map(
      (name, i): [string, number] => [name, i]);
  for (let rowNum = 0; rowNum < result.numRecords; rowNum++) {
    const row: Row = {};
    for (const [name, colNum] of columns) {
      row[name] = getCell(result, colNum, rowNum);
    }
    yield row;
  }
}

export {
  TraceConfig,
  TraceProcessor,
  IRawQueryArgs,
  RawQueryArgs,
  RawQueryResult,
};
