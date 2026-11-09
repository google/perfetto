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

import {ensureDefined} from '../../base/assert';
import {AtomicTaskQueue} from '../../base/async_memo';
import {defer} from '../../base/deferred';
import {okResult} from '../../base/result';
import type {Engine} from '../../trace_processor/engine';
import type {QueryResult} from '../../trace_processor/query_result';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import type {
  NodeData,
  NodeQueryBuilderStore,
  RootNodeData,
} from './graph_model';
import {flattenNodes} from './graph_utils';
import {MaterializationService} from './materialization_service';
import {planChunks} from './plan_chunks';
import {serializeChunks} from './chunk_queries';

function makeFakeEngine() {
  const queries: string[] = [];
  const engine = {
    async query(sql: string): Promise<QueryResult> {
      queries.push(sql);
      return {} as QueryResult;
    },
    async tryQuery(sql: string) {
      queries.push(sql);
      return okResult({} as QueryResult);
    },
  } as unknown as Engine;
  return {engine, queries};
}

function makeFilterChain(
  fromId: string,
  filterId: string,
  minDur: string,
): RootNodeData {
  const filter: NodeData = {
    type: 'filter',
    id: filterId,
    config: {
      conditions: [{column: 'dur', op: '>', value: minDur}],
    },
  };
  return {
    type: 'from',
    id: fromId,
    x: 0,
    y: 0,
    next: filter,
    config: {table: 'slice'},
  };
}

// From -> Filter -> Sort chain whose (expensive) Sort target is materialized
// into a cache table with the cheap Filter folded in.
function makeSortedChain(
  fromId: string,
  sortId: string,
  minDur: string,
): RootNodeData {
  const sort: NodeData = {
    type: 'sort',
    id: sortId,
    config: {conditions: [{column: 'dur', order: 'DESC'}]},
  };
  const filter: NodeData = {
    type: 'filter',
    id: `${sortId}_flt`,
    config: {
      conditions: [{column: 'dur', op: '>', value: minDur}],
    },
    next: sort,
  };
  return {
    type: 'from',
    id: fromId,
    x: 0,
    y: 0,
    next: filter,
    config: {table: 'slice'},
  };
}

describe('MaterializationService cache policy', () => {
  it('reads bare From nodes directly without creating any _qb_ cache table', async () => {
    const {engine, queries} = makeFakeEngine();
    const service = new MaterializationService(engine);

    const store: NodeQueryBuilderStore = {
      nodes: [
        {
          type: 'from',
          id: 'f1',
          x: 0,
          y: 0,
          config: {table: 'slice'},
        },
      ],
      labels: [],
    };

    await service.scheduleUpdate(store, 'f1', undefined, 'sql');
    expect(service.outputTable).toBe('slice');
    expect(service.cacheEntries.length).toBe(0);
    expect(queries.some((q) => q.includes('CREATE PERFETTO TABLE'))).toBe(
      false,
    );
  });

  it('caps total entries at maxEntries, keeping the most recently used', async () => {
    const {engine, queries} = makeFakeEngine();
    const service = new MaterializationService(engine, {maxEntries: 2});

    const store: NodeQueryBuilderStore = {
      nodes: [
        makeSortedChain('f1', 'srt1', '10'),
        makeSortedChain('f2', 'srt2', '20'),
        makeSortedChain('f3', 'srt3', '30'),
        makeSortedChain('f4', 'srt4', '40'),
      ],
      labels: [],
    };

    const hashes: string[] = [];
    for (const id of ['srt1', 'srt2', 'srt3', 'srt4']) {
      await service.scheduleUpdate(store, id, undefined);
      hashes.push(ensureDefined(service.outputTable));
      expect(service.cacheEntries.length).toBeLessThanOrEqual(2);
    }

    const cached = new Set(service.cacheEntries.map((e) => e.hash));
    expect(cached).toEqual(new Set([hashes[2], hashes[3]]));
    expect(queries).toContain(`DROP TABLE IF EXISTS ${hashes[0]}`);
    expect(queries).toContain(`DROP TABLE IF EXISTS ${hashes[1]}`);
  });

  it('evicts the least recently used entry first; a hit refreshes recency', async () => {
    let now = 1000;
    const nowSpy = vi.spyOn(performance, 'now').mockImplementation(() => {
      now += 1;
      return now;
    });

    try {
      const {engine, queries} = makeFakeEngine();
      const service = new MaterializationService(engine, {maxEntries: 2});
      const store: NodeQueryBuilderStore = {
        nodes: [
          makeSortedChain('fa', 'srta', '10'),
          makeSortedChain('fb', 'srtb', '20'),
          makeSortedChain('fc', 'srtc', '30'),
        ],
        labels: [],
      };

      await service.scheduleUpdate(store, 'srta', undefined);
      const hA = ensureDefined(service.outputTable);
      await service.scheduleUpdate(store, 'srtb', undefined);
      const hB = ensureDefined(service.outputTable);

      // Re-selecting A is a cache hit, making B the least recently used.
      queries.length = 0;
      await service.scheduleUpdate(store, 'srta', undefined);
      expect(queries.some((q) => q.includes('CREATE PERFETTO TABLE'))).toBe(
        false,
      );

      await service.scheduleUpdate(store, 'srtc', undefined);
      const hC = ensureDefined(service.outputTable);

      const cached = new Set(service.cacheEntries.map((e) => e.hash));
      expect(cached).toEqual(new Set([hA, hC]));
      expect(queries).toContain(`DROP TABLE IF EXISTS ${hB}`);
    } finally {
      nowSpy.mockRestore();
    }
  });

  it('never evicts a table the current output reads', async () => {
    const {engine} = makeFakeEngine();
    const service = new MaterializationService(engine, {maxEntries: 0});
    const store: NodeQueryBuilderStore = {
      nodes: [
        makeSortedChain('f1', 'srt1', '10'),
        makeSortedChain('f2', 'srt2', '20'),
      ],
      labels: [],
    };

    await service.scheduleUpdate(store, 'srt1', undefined);
    const h1 = ensureDefined(service.outputTable);
    expect(service.cacheEntries.map((e) => e.hash)).toEqual([h1]);

    // Switching away means h1 is no longer read, so it goes.
    await service.scheduleUpdate(store, 'srt2', undefined);
    const h2 = ensureDefined(service.outputTable);
    expect(service.cacheEntries.map((e) => e.hash)).toEqual([h2]);

    // With no selection nothing is read, so everything goes.
    await service.scheduleUpdate(store, undefined, undefined);
    expect(service.cacheEntries.length).toBe(0);
  });

  it('does not rebuild a plan with more steps than maxEntries', async () => {
    const {engine, queries} = makeFakeEngine();
    const service = new MaterializationService(engine, {maxEntries: 1});
    const store: NodeQueryBuilderStore = {
      nodes: [
        {
          type: 'from',
          id: 'f1',
          x: 0,
          y: 0,
          config: {table: 'slice'},
          next: {
            type: 'groupby',
            id: 'agg1',
            config: {
              groupColumns: ['name'],
              aggregations: [{func: 'COUNT', column: '*', alias: 'cnt'}],
            },
            next: {
              type: 'sort',
              id: 'srt1',
              config: {conditions: [{column: 'cnt', order: 'DESC'}]},
            },
          },
        },
        makeFilterChain('f2', 'flt2', '10'),
      ],
      labels: [],
    };

    // Two steps are built; the upstream one is evicted afterwards.
    await service.scheduleUpdate(store, 'srt1', undefined);
    const sortHash = ensureDefined(service.outputTable);
    expect(service.cacheEntries.map((e) => e.hash)).toEqual([sortHash]);

    // Select something else, then come back: the warm sort step is enough.
    await service.scheduleUpdate(store, 'flt2', undefined);
    queries.length = 0;
    await service.scheduleUpdate(store, 'srt1', undefined);
    expect(queries.some((q) => q.includes('CREATE PERFETTO TABLE'))).toBe(
      false,
    );
    expect(service.lastRunOutcomes.get(sortHash)?.kind).toBe('hit');
  });

  it('builds tables in PSN but reads a cheap output in Standard SQL', async () => {
    const {engine, queries} = makeFakeEngine();
    const service = new MaterializationService(engine);
    const store: NodeQueryBuilderStore = {
      nodes: [
        {
          ...makeSortedChain('f1', 'srt1', '10'),
        },
        {
          type: 'limit',
          id: 'lim1',
          x: 0,
          y: 0,
          inputs: ['srt1'],
          config: {limitCount: '5'},
        },
      ],
      labels: [],
    };

    await service.scheduleUpdate(store, 'lim1', undefined, 'pipe');
    expect(service.error).toBeUndefined();
    const [entry] = service.cacheEntries;
    const hash = ensureDefined(entry).hash;
    expect(
      queries.some((q) =>
        q.includes(
          `CREATE PERFETTO TABLE ${hash} AS\nFROM slice\n|> WHERE dur > 10\n|> ORDER BY dur DESC`,
        ),
      ),
    ).toBe(true);
    expect(service.outputRead?.sql).toBe(`SELECT *\nFROM ${hash}\nLIMIT 5`);
  });

  it("includes a chunk's modules in the statement that builds it", async () => {
    const {engine, queries} = makeFakeEngine();
    const service = new MaterializationService(engine);
    const sqlModules = {
      getModuleForTable: (table: string) =>
        table === 'thread_slice'
          ? {includeKey: 'slices.with_context'}
          : undefined,
      getTable: () => undefined,
    } as unknown as SqlModules;
    const chain = makeSortedChain('f1', 'srt1', '10');
    const store: NodeQueryBuilderStore = {
      nodes: [{...chain, config: {table: 'thread_slice'}}],
      labels: [],
    };

    await service.scheduleUpdate(store, 'srt1', sqlModules, 'sql');
    const hash = ensureDefined(service.outputTable);
    expect(queries).toContainEqual(
      [
        `DROP TABLE IF EXISTS ${hash};`,
        'INCLUDE PERFETTO MODULE slices.with_context;',
        `CREATE PERFETTO TABLE ${hash} AS`,
        'SELECT *',
        'FROM thread_slice',
        'WHERE dur > 10',
        'ORDER BY dur DESC',
      ].join('\n'),
    );
    expect(queries.some((q) => q.startsWith('INCLUDE'))).toBe(false);
  });

  it('does nothing when called again with the same inputs', async () => {
    const {engine, queries} = makeFakeEngine();
    const service = new MaterializationService(engine);
    const store: NodeQueryBuilderStore = {
      nodes: [makeSortedChain('f1', 'srt1', '10')],
      labels: [],
    };

    await service.scheduleUpdate(store, 'srt1', undefined);
    const plan = service.plan;
    expect(plan?.outputId).toBe('srt1');
    queries.length = 0;
    await service.scheduleUpdate(store, 'srt1', undefined);
    expect(queries).toEqual([]);
    expect(service.plan).toBe(plan);

    // Clearing the cache runs the latest update again.
    await service.clearCache();
    expect(queries.some((q) => q.includes('CREATE PERFETTO TABLE'))).toBe(true);
  });

  it('waits for in-flight taskQueue queries before dropping evicted tables', async () => {
    const events: string[] = [];
    const engine = {
      async query(sql: string): Promise<QueryResult> {
        events.push(`query:${sql.split('\n')[0]}`);
        return {} as QueryResult;
      },
      async tryQuery(sql: string) {
        events.push(`drop:${sql}`);
        return okResult({} as QueryResult);
      },
    } as unknown as Engine;

    const taskQueue = new AtomicTaskQueue();
    const service = new MaterializationService(engine, {
      maxEntries: 0,
      taskQueue,
    });

    const store1: NodeQueryBuilderStore = {
      nodes: [makeSortedChain('f1', 'srt1', '1')],
      labels: [],
    };
    await service.scheduleUpdate(store1, 'srt1', undefined);
    const h1 = ensureDefined(service.outputTable);

    // Simulate an in-flight SQLDataSource pagination query on taskQueue.
    const unblockRead = defer<void>();
    const readStarted = defer<void>();
    taskQueue.schedule({}, async () => {
      events.push(`read-start:${h1}`);
      readStarted.resolve();
      await unblockRead;
      events.push(`read-end:${h1}`);
    });
    await readStarted;

    // Trigger an edit that stops reading & evicts h1 while the read is still running.
    const store2: NodeQueryBuilderStore = {
      nodes: [makeSortedChain('f1', 'srt1', '2')],
      labels: [],
    };
    const updatePromise = service.scheduleUpdate(store2, 'srt1', undefined);

    // Let microtasks settle; drop must NOT have run yet because read is active.
    await Promise.resolve();
    expect(events).not.toContain(`drop:DROP TABLE IF EXISTS ${h1}`);

    unblockRead.resolve();
    await updatePromise;

    const readEndIdx = events.indexOf(`read-end:${h1}`);
    const dropIdx = events.indexOf(`drop:DROP TABLE IF EXISTS ${h1}`);
    expect(readEndIdx).toBeGreaterThanOrEqual(0);
    expect(dropIdx).toBeGreaterThan(readEndIdx);
  });

  it('plans cut points independently of cache state, materializing only expensive ops', async () => {
    const {engine, queries} = makeFakeEngine();
    const service = new MaterializationService(engine, {maxEntries: 4});

    // 1. User builds From -> Filter and clicks Filter -> cheap, so it is read
    // directly with no cache table.
    const step1Store: NodeQueryBuilderStore = {
      nodes: [makeFilterChain('f1', 'flt1', '500')],
      labels: [],
    };
    await service.scheduleUpdate(step1Store, 'flt1', undefined, 'sql');
    expect(service.cacheEntries.length).toBe(0);
    expect(service.outputRead?.sql).toBe(
      'SELECT *\nFROM slice\nWHERE dur > 500',
    );
    expect(queries.some((q) => q.includes('CREATE PERFETTO TABLE'))).toBe(
      false,
    );

    // 2. User docks Sort below Filter and clicks Sort -> the expensive Sort is
    // materialized with the Filter folded in.
    const step2Store: NodeQueryBuilderStore = {
      nodes: [
        {
          type: 'from',
          id: 'f1',
          x: 0,
          y: 0,
          config: {table: 'slice'},
          next: {
            type: 'filter',
            id: 'flt1',
            config: {conditions: [{column: 'dur', op: '>', value: '500'}]},
            next: {
              type: 'sort',
              id: 'srt1',
              config: {conditions: [{column: 'dur', order: 'DESC'}]},
            },
          },
        },
      ],
      labels: [],
    };
    const coldHashes = () => {
      const index = flattenNodes(step2Store.nodes);
      const res = serializeChunks(
        index,
        ensureDefined(planChunks(index, 'srt1')),
      );
      return res.ok ? res.chunks.map((c) => c.hash) : [];
    };
    const coldSteps = coldHashes();

    queries.length = 0;
    await service.scheduleUpdate(step2Store, 'srt1', undefined, 'sql');
    const sortHash = ensureDefined(service.outputTable);
    expect(coldSteps).toEqual([sortHash]);
    expect(service.lastRunOutcomes.get(sortHash)?.kind).toBe('built');
    expect(
      queries.some((q) =>
        q.includes(
          `CREATE PERFETTO TABLE ${sortHash} AS\nSELECT *\nFROM slice\nWHERE dur > 500\nORDER BY dur DESC`,
        ),
      ),
    ).toBe(true);

    // 3. Re-planning with a warm cache yields the identical plan.
    expect(coldHashes()).toEqual(coldSteps);

    // 4. Fork after the cheap Filter: `flt_shared` feeds `srt_a` and `lim_b`.
    // The shared Filter is not checkpointed; the cheap Limit branch reads
    // straight from `slice` with no cache table.
    const {engine: forkEngine} = makeFakeEngine();
    const forkService = new MaterializationService(forkEngine);
    const forkStore: NodeQueryBuilderStore = {
      nodes: [
        {
          type: 'from',
          id: 'f_shared',
          x: 0,
          y: 0,
          config: {table: 'slice'},
          next: {
            type: 'filter',
            id: 'flt_shared',
            config: {conditions: [{column: 'dur', op: '>', value: '999'}]},
            next: {
              type: 'sort',
              id: 'srt_a',
              config: {conditions: [{column: 'dur', order: 'DESC'}]},
            },
          },
        },
        {
          type: 'limit',
          id: 'lim_b',
          x: 200,
          y: 100,
          inputs: ['flt_shared'],
          config: {limitCount: '5'},
        },
      ],
      labels: [],
    };

    await forkService.scheduleUpdate(forkStore, 'srt_a', undefined, 'sql');
    expect(forkService.cacheEntries.length).toBe(1);

    await forkService.scheduleUpdate(forkStore, 'lim_b', undefined, 'sql');
    expect(forkService.cacheEntries.length).toBe(1);
    expect(forkService.outputRead?.sql).toBe(
      'SELECT *\nFROM slice\nWHERE dur > 999\nLIMIT 5',
    );
  });

  it('surfaces materialization error when a node is unsupported in Standard SQL mode', async () => {
    const {engine, queries} = makeFakeEngine();
    const service = new MaterializationService(engine, {maxEntries: 4});

    const stopRoot: RootNodeData = {
      type: 'from',
      id: 'stop1',
      x: 200,
      y: 0,
      config: {table: 'process'},
    };
    const evRoot: RootNodeData = {
      type: 'from',
      id: 'ev1',
      x: 0,
      y: 0,
      config: {table: 'counter'},
      next: {
        type: 'intervals_from_events',
        id: 'ife1',
        inputs: [null, 'stop1'],
        config: {
          partitionColumns: ['track_id'],
          lastMode: 'KEEP_OPEN',
          closeAtExpr: 'trace_end()',
        },
      },
    };
    const store: NodeQueryBuilderStore = {
      nodes: [stopRoot, evRoot],
      labels: [],
    };

    await service.scheduleUpdate(store, 'ife1', undefined, 'sql');
    expect(service.outputTable).toBeUndefined();
    expect(service.error).toBe(
      'Intervals From Events with a Stop input is only supported in PerfettoSQL Next mode.',
    );
    expect(queries.some((q) => q.includes('Unsupported in Standard SQL'))).toBe(
      false,
    );
  });
});
