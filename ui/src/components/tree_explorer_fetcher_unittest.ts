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
import {createDefaultTreeExplorerState} from '../widgets/tree_explorer';
import {TreeExplorerFetcher} from './tree_explorer_fetcher';
import type {TreeExplorerQueryMetric} from './tree_explorer_fetcher';

// What the operator query returns for a metric with no extra properties.
const TREE_COLUMNS =
  'id parentId depth name selfValue cumulativeValue parentCumulativeValue xStart xEnd'.split(
    ' ',
  );

function fakeQueryResult(
  columnNames: ReadonlyArray<string>,
  varintRow?: ReadonlyArray<number>,
): QueryResult {
  const cells = varintRow ?? [];
  const resProto = protos.QueryResult.create({
    columnNames: columnNames.slice(),
    batch: [
      protos.QueryResult.CellsBatch.create({
        cells: cells.map(
          () => protos.QueryResult.CellsBatch.CellType.CELL_VARINT,
        ),
        varintCells: cells.slice(),
        isLastBatch: true,
      }),
    ],
  });
  const res = createQueryResult({query: ''});
  res.appendResultBatch(protos.QueryResult.encode(resProto).finish());
  return res;
}

// A fake engine logging every statement, so tests can assert CREATE/DROPs.
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

function metric(name: string): TreeExplorerQueryMetric {
  return {
    name,
    unit: '',
    statement: `select 0 as id, null as parentId, '${name}' as name, 0 as value`,
  };
}

function creates(log: ReadonlyArray<string>): string[] {
  return log.filter((sql) => sql.startsWith('CREATE VIRTUAL TABLE'));
}

function drops(log: ReadonlyArray<string>): string[] {
  return log.filter((sql) => sql.startsWith('DROP TABLE'));
}

describe('TreeExplorerFetcher table cache', () => {
  test('reuses the table for a metric already in the current set', async () => {
    const log: string[] = [];
    const fetcher = new TreeExplorerFetcher(fakeTrace(log));
    const metrics = [metric('m1'), metric('m2')];
    const state = createDefaultTreeExplorerState(metrics);

    await fetcher.fetch(metrics, state);
    expect(creates(log)).toHaveLength(1);

    await fetcher.fetch(metrics, {...state, selectedMetricId: 'm2'});
    expect(creates(log)).toHaveLength(2);
    expect(drops(log)).toHaveLength(0);

    await fetcher.fetch(metrics, state);
    expect(creates(log)).toHaveLength(2);
    expect(drops(log)).toHaveLength(0);
  });

  test('evicts tables for metrics absent from the new set', async () => {
    const log: string[] = [];
    const fetcher = new TreeExplorerFetcher(fakeTrace(log));
    const metricsA = [metric('m1'), metric('m2')];
    const stateA = createDefaultTreeExplorerState(metricsA);

    await fetcher.fetch(metricsA, stateA);
    await fetcher.fetch(metricsA, {...stateA, selectedMetricId: 'm2'});
    expect(creates(log)).toHaveLength(2);

    const metricsB = [metric('m3')];
    await fetcher.fetch(metricsB, createDefaultTreeExplorerState(metricsB));
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
    const fetcher = new TreeExplorerFetcher(fakeTrace(log));
    const metrics = [metric('m1'), metric('m2')];
    const state = createDefaultTreeExplorerState(metrics);

    await fetcher.fetch(metrics, state);
    await fetcher.fetch(metrics, {...state, selectedMetricId: 'm2'});

    await fetcher[Symbol.asyncDispose]();
    expect(drops(log)).toHaveLength(2);
  });
});
