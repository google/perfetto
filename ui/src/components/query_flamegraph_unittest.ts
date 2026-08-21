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

import {describe, expect, test} from 'vitest';

import protos from '../protos';
import {createQueryResult} from '../trace_processor/query_result';
import type {QueryResult} from '../trace_processor/query_result';
import type {Trace} from '../public/trace';
import {Flamegraph} from '../widgets/flamegraph';
import {QueryFlamegraph} from './query_flamegraph';
import type {QueryFlamegraphMetric} from './query_flamegraph';

// Columns produced by the flamegraph operator query for a metric with no
// extra properties (see computeFlamegraphTree).
const TREE_COLUMNS = [
  'id',
  'parentId',
  'depth',
  'name',
  'selfValue',
  'cumulativeValue',
  'parentCumulativeValue',
  'xStart',
  'xEnd',
];

function fakeQueryResult(
  columnNames: ReadonlyArray<string>,
  varintRow?: ReadonlyArray<number>,
): QueryResult {
  const cellType = protos.QueryResult.CellsBatch.CellType;
  const batch =
    varintRow !== undefined
      ? protos.QueryResult.CellsBatch.create({
          cells: varintRow.map(() => cellType.CELL_VARINT),
          varintCells: varintRow.slice(),
          isLastBatch: true,
        })
      : protos.QueryResult.CellsBatch.create({cells: [], isLastBatch: true});
  const resProto = protos.QueryResult.create({
    columnNames: columnNames.slice(),
    batch: [batch],
  });
  const res = createQueryResult({query: ''});
  res.appendResultBatch(protos.QueryResult.encode(resProto).finish());
  return res;
}

// A fake trace whose engine responds to exactly the SQL that QueryFlamegraph
// issues, recording every statement so tests can assert on the CREATE/DROP
// traffic.
function fakeTrace(log: string[]): Trace {
  const respond = (sql: string): QueryResult => {
    log.push(sql);
    if (sql.includes('__intrinsic_flamegraph_find')) {
      return fakeQueryResult(['cumulative_value'], [0]);
    }
    if (sql.includes('cumulative_value > 0')) {
      return fakeQueryResult(TREE_COLUMNS);
    }
    return fakeQueryResult([]);
  };
  const engine = {
    query: async (sql: string) => respond(sql),
    tryQuery: async (sql: string) => {
      respond(sql);
      return {ok: true};
    },
  };
  return {engine} as unknown as Trace;
}

function metric(name: string): QueryFlamegraphMetric {
  return {
    name,
    unit: '',
    statement: `select 0 as id, null as parentId, '${name}' as name, 0 as value`,
  };
}

// All fake queries resolve in microtasks; a couple of macrotask turns is
// enough for the AsyncLimiter chain to drain.
async function settle(): Promise<void> {
  for (let i = 0; i < 20; i++) {
    await new Promise((r) => setTimeout(r, 0));
  }
}

function creates(log: ReadonlyArray<string>): string[] {
  return log.filter((sql) => sql.startsWith('CREATE VIRTUAL TABLE'));
}

function drops(log: ReadonlyArray<string>): string[] {
  return log.filter((sql) => sql.startsWith('DROP TABLE'));
}

describe('QueryFlamegraph table cache', () => {
  test('reuses the table for a metric already in the current set', async () => {
    const log: string[] = [];
    const qf = new QueryFlamegraph(fakeTrace(log));
    const metrics = [metric('m1'), metric('m2')];

    qf.fetchData(metrics, Flamegraph.createDefaultState(metrics));
    await settle();
    expect(creates(log)).toHaveLength(1);

    // Select the second metric: one more table, nothing dropped.
    qf.fetchData(metrics, {
      ...Flamegraph.createDefaultState(metrics),
      selectedMetricId: 'm2',
    });
    await settle();
    expect(creates(log)).toHaveLength(2);
    expect(drops(log)).toHaveLength(0);

    // Back to the first metric: cache hit, no new table.
    qf.fetchData(metrics, Flamegraph.createDefaultState(metrics));
    await settle();
    expect(creates(log)).toHaveLength(2);
    expect(drops(log)).toHaveLength(0);
  });

  test('evicts tables for metrics absent from the new set', async () => {
    const log: string[] = [];
    const qf = new QueryFlamegraph(fakeTrace(log));
    const metricsA = [metric('m1'), metric('m2')];

    qf.fetchData(metricsA, Flamegraph.createDefaultState(metricsA));
    await settle();
    qf.fetchData(metricsA, {
      ...Flamegraph.createDefaultState(metricsA),
      selectedMetricId: 'm2',
    });
    await settle();
    expect(creates(log)).toHaveLength(2);

    // A new metric array: both m1 and m2 tables must be dropped.
    const metricsB = [metric('m3')];
    qf.fetchData(metricsB, Flamegraph.createDefaultState(metricsB));
    await settle();
    expect(creates(log)).toHaveLength(3);
    expect(drops(log)).toHaveLength(2);

    // The dropped names are exactly the ones created for the old set.
    const createdNames = creates(log).map(
      (sql) => sql.split(' ')[3], // CREATE VIRTUAL TABLE <name> USING ...
    );
    const droppedNames = drops(log).map(
      (sql) => sql.split(' ')[4], // DROP TABLE IF EXISTS <name>
    );
    expect(droppedNames.sort()).toEqual(createdNames.slice(0, 2).sort());
  });

  test('disposal drops every cached table', async () => {
    const log: string[] = [];
    const qf = new QueryFlamegraph(fakeTrace(log));
    const metrics = [metric('m1'), metric('m2')];

    qf.fetchData(metrics, Flamegraph.createDefaultState(metrics));
    await settle();
    qf.fetchData(metrics, {
      ...Flamegraph.createDefaultState(metrics),
      selectedMetricId: 'm2',
    });
    await settle();

    await qf[Symbol.asyncDispose]();
    expect(drops(log)).toHaveLength(2);
  });
});
