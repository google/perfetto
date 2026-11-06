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
import type {
  NodeData,
  NodeQueryBuilderStore,
  RootNodeData,
} from './graph_model';
import {flattenNodes} from './graph_utils';
import {buildLogicalPlan, collectLiveHashes, type LogicalPlan} from './ir';
import {
  computeNeededStepHashes,
  MaterializationService,
  planMaterialization,
} from './materialization';

function nodeHash(plan: LogicalPlan, id: string): string {
  return ensureDefined(plan.nodes.find((n) => n.nodeId === id)).hash;
}

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

describe('collectLiveHashes', () => {
  it('includes valid non-scan Merkle hashes in a chain without materializing bare scans', () => {
    const sort: NodeData = {
      type: 'sort',
      id: 'sort1',
      config: {
        conditions: [{column: 'ts', order: 'ASC'}],
      },
    };
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      next: sort,
      config: {
        conditions: [{column: 'dur', op: '>', value: '100'}],
      },
    };
    const from: RootNodeData = {
      type: 'from',
      id: 'from1',
      x: 0,
      y: 0,
      next: filter,
      config: {table: 'slice'},
    };

    const index = flattenNodes([from]);
    const plan = ensureDefined(buildLogicalPlan(index, 'sort1', undefined));
    const fromHash = nodeHash(plan, 'from1');
    const filterHash = nodeHash(plan, 'flt1');
    const sortHash = nodeHash(plan, 'sort1');

    const live = collectLiveHashes(index, undefined);
    expect(live.has(fromHash)).toBe(false);
    expect(live.has(filterHash)).toBe(true);
    expect(live.has(sortHash)).toBe(true);
    expect(live.size).toBe(2);
  });

  it('skips invalid nodes while keeping valid branches', () => {
    const validBranch = makeFilterChain('f1', 'flt1', '50');
    const invalidFilter: NodeData = {
      type: 'filter',
      id: 'flt_bad',
      config: {
        conditions: [{column: 'dur', op: '>', value: ''}],
      },
    };
    const invalidBranch: RootNodeData = {
      type: 'from',
      id: 'f2',
      x: 100,
      y: 0,
      next: invalidFilter,
      config: {table: 'sched'},
    };

    const index = flattenNodes([validBranch, invalidBranch]);
    const live = collectLiveHashes(index, undefined);

    const plan = ensureDefined(buildLogicalPlan(index, 'flt1', undefined));
    const validHash = nodeHash(plan, 'flt1');

    expect(live.has(validHash)).toBe(true);
    expect(live.size).toBe(1);
  });
});

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
    expect(service.materializedTable).toBe('slice');
    expect(service.cacheEntries.length).toBe(0);
    expect(queries.some((q) => q.includes('CREATE PERFETTO TABLE'))).toBe(
      false,
    );
  });

  it('never evicts live graph hashes regardless of orphan churn', async () => {
    const {engine} = makeFakeEngine();
    const service = new MaterializationService(engine, {maxOrphans: 2});

    // Build 4 independent live branches (> maxOrphans) and materialize them all.
    const liveBranches = [
      makeFilterChain('f1', 'flt1', '10'),
      makeFilterChain('f2', 'flt2', '20'),
      makeFilterChain('f3', 'flt3', '30'),
      makeFilterChain('f4', 'flt4', '40'),
    ];
    let store: NodeQueryBuilderStore = {
      nodes: [...liveBranches, makeFilterChain('fe', 'flte', '100')],
      labels: [],
    };

    for (const id of ['flt1', 'flt2', 'flt3', 'flt4']) {
      await service.scheduleUpdate(store, id, undefined);
    }
    const pinnedHashes = service.cacheEntries.map((e) => e.hash);
    expect(pinnedHashes.length).toBe(4);

    // Rapidly edit branch 'flte' 5 times, leaving 4 orphans behind while the
    // 5th edit is live. Only maxOrphans (2) orphans should remain, while all
    // 4 untouched live branches stay cached.
    for (const val of ['101', '102', '103', '104', '105']) {
      store = {
        nodes: [...liveBranches, makeFilterChain('fe', 'flte', val)],
        labels: [],
      };
      await service.scheduleUpdate(store, 'flte', undefined);
    }

    const remainingHashes = new Set(service.cacheEntries.map((e) => e.hash));
    for (const h of pinnedHashes) {
      expect(remainingHashes.has(h)).toBe(true);
    }

    const liveEntries = service.cacheEntries.filter((e) => e.isLive);
    const orphanEntries = service.cacheEntries.filter((e) => !e.isLive);
    expect(liveEntries.length).toBe(5); // 4 untouched branches + latest flte
    expect(orphanEntries.length).toBe(2); // capped at maxOrphans
  });

  it('evicts lower-priority orphans first and retains frequently hit or expensive orphans (GreedyDual)', async () => {
    let simulatedCostMs = 1;
    let now = 1000;
    const queries: string[] = [];
    const engine = {
      async query(sql: string): Promise<QueryResult> {
        queries.push(sql);
        now += simulatedCostMs;
        return {} as QueryResult;
      },
      async tryQuery(sql: string) {
        queries.push(sql);
        return okResult({} as QueryResult);
      },
    } as unknown as Engine;

    const nowSpy = vi.spyOn(performance, 'now').mockImplementation(() => {
      now += 0.1;
      return now;
    });

    try {
      const service = new MaterializationService(engine, {maxOrphans: 2});
      const storeFor = (val: string): NodeQueryBuilderStore => ({
        nodes: [makeFilterChain('f1', 'flt1', val)],
        labels: [],
      });

      // Materialize expensive query '1' (100ms).
      simulatedCostMs = 100;
      await service.scheduleUpdate(storeFor('1'), 'flt1', undefined);
      const hExpensive = ensureDefined(service.materializedTable);

      // Materialize cheap queries '2' (1ms) and '3' (1ms).
      simulatedCostMs = 1;
      await service.scheduleUpdate(storeFor('2'), 'flt1', undefined);
      const hCheapOld = ensureDefined(service.materializedTable);

      await service.scheduleUpdate(storeFor('3'), 'flt1', undefined);
      const hCheapNewer = ensureDefined(service.materializedTable);

      // Edit to '4' -> 3 orphans (`hExpensive`, `hCheapOld`, `hCheapNewer`), maxOrphans = 2.
      // Even though `hExpensive` was created earliest, its high materialization
      // cost gives it higher GreedyDual priority than `hCheapOld`, so `hCheapOld`
      // is evicted first!
      await service.scheduleUpdate(storeFor('4'), 'flt1', undefined);

      const cachedHashes = new Set(service.cacheEntries.map((e) => e.hash));
      expect(cachedHashes.has(hExpensive)).toBe(true);
      expect(cachedHashes.has(hCheapNewer)).toBe(true);
      expect(cachedHashes.has(hCheapOld)).toBe(false);
      expect(queries).toContain(`DROP TABLE IF EXISTS ${hCheapOld}`);
    } finally {
      nowSpy.mockRestore();
    }
  });

  it('sweeps orphans when nodes are deleted with no active selection or while another node is pinned', async () => {
    const {engine} = makeFakeEngine();
    const service = new MaterializationService(engine, {maxOrphans: 1});

    const branchA = makeFilterChain('fa', 'flta', '10');
    const branchB = makeFilterChain('fb', 'fltb', '20');
    const branchC = makeFilterChain('fc', 'fltc', '30');

    const fullStore: NodeQueryBuilderStore = {
      nodes: [branchA, branchB, branchC],
      labels: [],
    };
    await service.scheduleUpdate(fullStore, 'flta', undefined);
    await service.scheduleUpdate(fullStore, 'fltb', undefined);
    await service.scheduleUpdate(fullStore, 'fltc', undefined);
    await service.scheduleUpdate(fullStore, 'flta', undefined);
    expect(service.cacheEntries.length).toBe(3);

    // Keep flta active (same displaySql!), delete branchB and branchC -> 2 orphans > maxOrphans (1).
    const trimmedStore: NodeQueryBuilderStore = {
      nodes: [branchA],
      labels: [],
    };
    await service.scheduleUpdate(trimmedStore, 'flta', undefined);
    expect(service.cacheEntries.length).toBe(2); // flta (live) + 1 orphan

    // Clear workspace completely (activeNodeId = undefined) -> only 1 orphan allowed.
    const emptyStore: NodeQueryBuilderStore = {nodes: [], labels: []};
    await service.scheduleUpdate(emptyStore, undefined, undefined);
    expect(service.cacheEntries.length).toBe(1);
    expect(service.cacheEntries[0]?.isLive).toBe(false);

    // Prune orphans drops remaining orphan immediately.
    await service.pruneOrphans();
    expect(service.cacheEntries.length).toBe(0);
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
      maxOrphans: 0,
      taskQueue,
    });

    const store1: NodeQueryBuilderStore = {
      nodes: [makeFilterChain('f1', 'flt1', '1')],
      labels: [],
    };
    await service.scheduleUpdate(store1, 'flt1', undefined);
    const h1 = ensureDefined(service.materializedTable);

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

    // Trigger an edit that orphans & evicts h1 while the read is still running.
    const store2: NodeQueryBuilderStore = {
      nodes: [makeFilterChain('f1', 'flt1', '2')],
      labels: [],
    };
    const updatePromise = service.scheduleUpdate(store2, 'flt1', undefined);

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

  it('materializes with CREATE PERFETTO TABLE and shares warm cache across Standard SQL and PerfettoSQL Next dialects', async () => {
    const {engine, queries} = makeFakeEngine();
    const service = new MaterializationService(engine, {maxOrphans: 4});

    const store: NodeQueryBuilderStore = {
      nodes: [makeFilterChain('f1', 'flt1', '250')],
      labels: [],
    };

    // Materialize in Standard SQL mode first -> uses CREATE PERFETTO TABLE.
    await service.scheduleUpdate(store, 'flt1', undefined, 'sql');
    const sqlHash = ensureDefined(service.materializedTable);
    expect(service.activeDialect).toBe('sql');
    expect(service.cacheEntries[0]?.hitCount).toBe(1);
    expect(
      queries.some((q) =>
        q.includes(
          `CREATE PERFETTO TABLE ${sqlHash} AS\nSELECT *\nFROM slice\nWHERE dur > 250`,
        ),
      ),
    ).toBe(true);

    // Switch to PerfettoSQL Next mode -> same semantic Merkle hash -> instant
    // warm cache hit with ZERO new CREATE PERFETTO TABLE queries!
    const queryCountBeforeSwitch = queries.length;
    await service.scheduleUpdate(store, 'flt1', undefined, 'pipe');
    expect(service.activeDialect).toBe('pipe');
    expect(service.materializedTable).toBe(sqlHash);
    expect(service.cacheEntries[0]?.hitCount).toBe(2);
    expect(service.cacheEntries[0]?.sql).toBe('FROM slice\n|> WHERE dur > 250');
    expect(
      queries
        .slice(queryCountBeforeSwitch)
        .some((q) => q.includes('CREATE PERFETTO TABLE')),
    ).toBe(false);
  });

  it('reuses warm upstream prefix when docking new nodes interactively and shares Y-fork checkpoints across branches', async () => {
    const {engine, queries} = makeFakeEngine();
    const service = new MaterializationService(engine, {maxOrphans: 4});

    // 1. User builds From -> Filter and clicks Filter.
    const step1Store: NodeQueryBuilderStore = {
      nodes: [makeFilterChain('f1', 'flt1', '500')],
      labels: [],
    };
    await service.scheduleUpdate(step1Store, 'flt1', undefined, 'sql');
    const filterHash = ensureDefined(service.materializedTable);
    expect(service.lastRunOutcomes.get(filterHash)?.kind).toBe('built');

    // 2. User docks Sort below Filter and clicks Sort -> Sort reads from
    // `_qb_<filterHash>` instead of re-scanning `slice`!
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
    queries.length = 0;
    await service.scheduleUpdate(step2Store, 'srt1', undefined, 'sql');
    const sortHash = ensureDefined(service.materializedTable);
    expect(service.lastRunOutcomes.get(filterHash)?.kind).toBe('hit');
    expect(service.lastRunOutcomes.get(sortHash)?.kind).toBe('built');
    expect(
      service.cacheEntries.find((e) => e.hash === filterHash)?.hitCount,
    ).toBe(2);
    expect(
      queries.some((q) =>
        q.includes(
          `CREATE PERFETTO TABLE ${sortHash} AS\nSELECT *\nFROM ${filterHash}\nORDER BY dur DESC`,
        ),
      ),
    ).toBe(true);

    // 3. Y-fork on a fresh service: `flt_shared` feeds `srt_a` and `lim_b`.
    const {engine: forkEngine, queries: forkQueries} = makeFakeEngine();
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

    // Selecting `srt_a` checkpoints `flt_shared` first because `canvasRefCounts` = 2.
    await forkService.scheduleUpdate(forkStore, 'srt_a', undefined, 'sql');
    expect(forkService.cacheEntries.length).toBe(2);
    const sharedHash = ensureDefined(
      forkService.cacheEntries.find(
        (e) => e.hash !== forkService.materializedTable,
      )?.hash,
    );

    // Selecting sibling `lim_b` hits `sharedHash` in cache!
    forkQueries.length = 0;
    await forkService.scheduleUpdate(forkStore, 'lim_b', undefined, 'sql');
    expect(
      forkService.cacheEntries.find((e) => e.hash === sharedHash)?.hitCount,
    ).toBe(2);
    expect(
      forkQueries.some((q) => q.includes(`FROM ${sharedHash}\nLIMIT 5`)),
    ).toBe(true);
  });

  it('surfaces materialization error when a node is unsupported in Standard SQL mode', async () => {
    const {engine, queries} = makeFakeEngine();
    const service = new MaterializationService(engine, {maxOrphans: 4});

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
    expect(service.materializedTable).toBeUndefined();
    expect(service.error).toBe(
      'Intervals From Events with a Stop input is only supported in PerfettoSQL Next mode.',
    );
    expect(queries.some((q) => q.includes('Unsupported in Standard SQL'))).toBe(
      false,
    );
  });

  it('retains full step DAG when downstream target is warm while bypassing evicted ancestors during execution', () => {
    const chain: RootNodeData = {
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
    };
    const index = flattenNodes([chain]);
    const logicalPlan = ensureDefined(
      buildLogicalPlan(index, 'srt1', undefined),
    );
    const aggHash = nodeHash(logicalPlan, 'agg1');
    const sortHash = nodeHash(logicalPlan, 'srt1');

    // Even when `sortHash` is warm and `aggHash` has been evicted,
    // `planMaterialization` still exposes both steps and their `deps` edge for
    // the IR graph, while `computeNeededStepHashes` stops at `sortHash`.
    const matPlan = planMaterialization(logicalPlan, {
      warmHashes: new Set([sortHash]),
    });
    expect(matPlan.steps.map((s) => s.hash)).toEqual([aggHash, sortHash]);
    expect(matPlan.steps.find((s) => s.hash === sortHash)?.deps).toEqual([
      aggHash,
    ]);

    const needed = computeNeededStepHashes(matPlan, new Set([sortHash]));
    expect([...needed]).toEqual([sortHash]);
  });
});
