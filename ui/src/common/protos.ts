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
import IProcessStatsConfig = protos.perfetto.protos.IProcessStatsConfig;
import IRawQueryArgs = protos.perfetto.protos.IRawQueryArgs;
import ISysStatsConfig = protos.perfetto.protos.ISysStatsConfig;
import ITraceConfig = protos.perfetto.protos.ITraceConfig;
import MeminfoCounters = protos.perfetto.protos.MeminfoCounters;
import RawQueryArgs = protos.perfetto.protos.RawQueryArgs;
import RawQueryResult = protos.perfetto.protos.RawQueryResult;
import StatCounters = protos.perfetto.protos.SysStatsConfig.StatCounters;
import TraceConfig = protos.perfetto.protos.TraceConfig;
import TraceProcessor = protos.perfetto.protos.TraceProcessor;
import VmstatCounters = protos.perfetto.protos.VmstatCounters;

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
  // Two columns can conflict on the same name, e.g.
  // select x.foo, y.foo from x join y. In that case store them using the
  // full table.column notation.
  const res = [] as string[];
  const uniqColNames = new Set<string>();
  const colNamesToDedupe = new Set<string>();
  for (const col of result.columnDescriptors) {
    const colName = col.name || '';
    if (uniqColNames.has(colName)) {
      colNamesToDedupe.add(colName);
    }
    uniqColNames.add(colName);
  }
  for (let i = 0; i < result.columnDescriptors.length; i++) {
    const colName = result.columnDescriptors[i].name || '';
    if (colNamesToDedupe.has(colName)) {
      res.push(`${colName}.${i + 1}`);
    } else {
      res.push(colName);
    }
  }
  return res;
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
  IProcessStatsConfig,
  IRawQueryArgs,
  ISysStatsConfig,
  ITraceConfig,
  MeminfoCounters,
  RawQueryArgs,
  RawQueryResult,
  StatCounters,
  TraceConfig,
  TraceProcessor,
  VmstatCounters,
};
