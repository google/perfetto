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
import {buildIR, collectLiveHashes} from './ir';
import {MaterializationService} from './materialization';

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
  it('includes both intermediate and folded hashes in a chain', () => {
    const sort: NodeData = {
      type: 'sort',
      id: 'sort1',
      config: {
        conditions: [{column: 'ts', direction: 'ASC'}],
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
    const fromHash = ensureDefined(
      ensureDefined(buildIR(index, 'from1', undefined))[0],
    ).hash;
    const filterHash = ensureDefined(
      ensureDefined(buildIR(index, 'flt1', undefined))[0],
    ).hash;
    const sortHash = ensureDefined(
      ensureDefined(buildIR(index, 'sort1', undefined))[0],
    ).hash;

    const live = collectLiveHashes(index, undefined);
    expect(live.has(fromHash)).toBe(true);
    expect(live.has(filterHash)).toBe(true);
    expect(live.has(sortHash)).toBe(true);
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

    const validHash = ensureDefined(
      ensureDefined(buildIR(index, 'flt1', undefined))[0],
    ).hash;
    const f2Hash = ensureDefined(
      ensureDefined(buildIR(index, 'f2', undefined))[0],
    ).hash;

    expect(live.has(validHash)).toBe(true);
    expect(live.has(f2Hash)).toBe(true);
    expect(live.size).toBe(3); // f1, flt1, f2
  });
});

describe('MaterializationService cache policy', () => {
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

  it('evicts least-recently-hit orphans first', async () => {
    const {engine, queries} = makeFakeEngine();
    const service = new MaterializationService(engine, {maxOrphans: 2});

    let now = 1000;
    const nowSpy = vi.spyOn(performance, 'now').mockImplementation(() => {
      now += 10;
      return now;
    });

    try {
      const storeFor = (val: string): NodeQueryBuilderStore => ({
        nodes: [makeFilterChain('f1', 'flt1', val)],
        labels: [],
      });

      await service.scheduleUpdate(storeFor('1'), 'flt1', undefined);
      const h1 = ensureDefined(service.materializedTable);

      await service.scheduleUpdate(storeFor('2'), 'flt1', undefined);
      const h2 = ensureDefined(service.materializedTable);

      // Undo back to '1' -> cache hit refreshes h1's lastHitAt ahead of h2.
      await service.scheduleUpdate(storeFor('1'), 'flt1', undefined);
      expect(service.queryReport?.entries[0]?.cacheHit).toBe(true);

      // Edit to '3' -> h1 and h2 are both orphans (2 <= maxOrphans, both kept).
      await service.scheduleUpdate(storeFor('3'), 'flt1', undefined);

      // Edit to '4' -> 3 orphans (h2 oldest, h1 refreshed, h3 newest); h2 is evicted.
      await service.scheduleUpdate(storeFor('4'), 'flt1', undefined);

      const cachedHashes = new Set(service.cacheEntries.map((e) => e.hash));
      expect(cachedHashes.has(h2)).toBe(false);
      expect(cachedHashes.has(h1)).toBe(true);
      expect(queries).toContain(`DROP TABLE IF EXISTS ${h2}`);
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
});
