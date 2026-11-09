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

import m from 'mithril';
import {AsyncLimiter} from '../../base/async_limiter';
import {AtomicTaskQueue} from '../../base/async_memo';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import type {Engine} from '../../trace_processor/engine';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import type {NodeQueryBuilderStore} from './graph_model';
import {flattenNodes, type GraphIndex} from './graph_utils';
import type {SqlDialect} from './sql_types';
import {type ChunkPlan, planChunks} from './plan_chunks';
import {
  type ChunkQuery,
  isOutputMaterialized,
  outputQuery,
  serializeChunks,
} from './chunk_queries';
import {type CacheEntry, runChunks, type StepRunOutcome} from './run_chunks';

const DEFAULT_MAX_ENTRIES = 16;

export interface MaterializationOptions {
  /** Maximum number of cache tables kept (least recently used evicted). */
  readonly maxEntries?: number;
  readonly taskQueue?: AtomicTaskQueue;
}

interface StoredCacheEntry {
  readonly hash: string;
  readonly sql: string;
  readonly createdAt: number;
  readonly materializeTimeMs: number;
  lastHitAt: number;
  hitCount: number;
}

export class MaterializationService {
  private readonly limiter = new AsyncLimiter();
  private readonly engine: Engine;
  private readonly taskQueue: AtomicTaskQueue;
  private readonly maxEntries: number;

  private cache = new Map<string, StoredCacheEntry>();
  // Cache tables the current output query reads: never evicted.
  private readHashes: ReadonlySet<string> = new Set();

  private latestUpdate:
    | {
        readonly store: NodeQueryBuilderStore;
        readonly activeNodeId: string | undefined;
        readonly sqlModules: SqlModules | undefined;
        readonly dialect: SqlDialect;
        readonly emulateSlowLoads: boolean;
      }
    | undefined;
  private pendingUpdate = false;
  private pendingClearAll = false;

  // Identifies the last run (dialect plus every chunk's hash and query), so an
  // unchanged graph isn't run again.
  private lastRunKey: string | undefined;
  private _error: string | undefined;
  private _dataSource: SQLDataSource | undefined;
  private _readQuerySql: string | undefined;
  private _materializedTable: string | undefined;

  private _lastRunOutcomes: ReadonlyMap<string, StepRunOutcome> = new Map();
  private _nodeOutcomes: ReadonlyMap<string, StepRunOutcome> = new Map();

  private disposed = false;

  constructor(engine: Engine, options?: MaterializationOptions) {
    this.engine = engine;
    this.maxEntries = options?.maxEntries ?? DEFAULT_MAX_ENTRIES;
    this.taskQueue = options?.taskQueue ?? new AtomicTaskQueue();
  }

  get dataSource(): SQLDataSource | undefined {
    return this._dataSource;
  }

  get readQuerySql(): string | undefined {
    return this._readQuerySql;
  }

  get materializedTable(): string | undefined {
    return this._materializedTable;
  }

  get error(): string | undefined {
    return this._error;
  }

  get cacheEntries(): readonly CacheEntry[] {
    return Array.from(this.cache.values(), (entry) => ({
      hash: entry.hash,
      sql: entry.sql,
      createdAt: entry.createdAt,
      materializeTimeMs: entry.materializeTimeMs,
      lastHitAt: entry.lastHitAt,
      hitCount: entry.hitCount,
    }));
  }

  /** The last run's outcome for each node, by the step it belongs to. */
  get nodeOutcomes(): ReadonlyMap<string, StepRunOutcome> {
    return this._nodeOutcomes;
  }

  get lastRunOutcomes(): ReadonlyMap<string, StepRunOutcome> {
    return this._lastRunOutcomes;
  }

  scheduleUpdate(
    store: NodeQueryBuilderStore,
    activeNodeId: string | undefined,
    sqlModules: SqlModules | undefined,
    dialect: SqlDialect = 'sql',
    emulateSlowLoads: boolean = false,
  ): Promise<void> {
    if (this.disposed) return Promise.resolve();
    this.latestUpdate = {
      store,
      activeNodeId,
      sqlModules,
      dialect,
      emulateSlowLoads,
    };
    this.pendingUpdate = true;
    return this.scheduleWork();
  }

  clearCache(): Promise<void> {
    if (this.disposed) return Promise.resolve();
    this.pendingClearAll = true;
    return this.scheduleWork();
  }

  private async scheduleWork(): Promise<void> {
    if (this.disposed) return;

    await this.limiter.schedule(async () => {
      if (this.disposed) return;

      if (this.pendingClearAll) {
        this.pendingClearAll = false;
        this.clearActiveOutput();
        await this.dropAll();
        m.redraw();
      }

      if (this.disposed || !this.pendingUpdate || !this.latestUpdate) return;
      this.pendingUpdate = false;
      const {store, activeNodeId, sqlModules, dialect} = this.latestUpdate;
      await this.doUpdate(store, activeNodeId, sqlModules, dialect);
    });
  }

  private clearActiveOutput(): void {
    this._dataSource?.dispose();
    this._dataSource = undefined;
    this._readQuerySql = undefined;
    this._materializedTable = undefined;
    this.readHashes = new Set();
    this.lastRunKey = undefined;
    this._error = undefined;
    this._lastRunOutcomes = new Map();
    this._nodeOutcomes = new Map();
  }

  private async doUpdate(
    store: NodeQueryBuilderStore,
    activeNodeId: string | undefined,
    sqlModules: SqlModules | undefined,
    dialect: SqlDialect,
  ): Promise<void> {
    const index = flattenNodes(store.nodes);
    const plan = activeNodeId ? planChunks(index, activeNodeId) : undefined;

    if (!plan) {
      this.clearActiveOutput();
      await this.evict();
      return;
    }

    const queries = serializeChunks(index, plan, {sqlModules, dialect});
    // Every chunk's hash names its exact query, so they identify the run.
    const runKey = [
      dialect,
      ...(queries.ok
        ? queries.chunks.map((c) => `${c.hash}\n${c.sql}`)
        : [`-- ${queries.error}`]),
    ].join('\n');

    if (
      runKey === this.lastRunKey &&
      (this._dataSource !== undefined || this._error !== undefined)
    ) {
      await this.evict();
      return;
    }

    this.lastRunKey = runKey;

    try {
      if (!queries.ok) throw new Error(queries.error);
      await this.executeChunks(index, plan, queries.chunks, sqlModules);
      this._error = undefined;
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      console.error('[QueryBuilder] Materialization error:', msg);
      this._error = msg;
      this._dataSource?.dispose();
      this._dataSource = undefined;
      this._readQuerySql = undefined;
      this._materializedTable = undefined;
      this.readHashes = new Set();
      this._lastRunOutcomes = new Map();
      this._nodeOutcomes = new Map();
      m.redraw();
    }

    await this.evict();
  }

  private async executeChunks(
    index: GraphIndex,
    plan: ChunkPlan,
    chunks: readonly ChunkQuery[],
    sqlModules: SqlModules | undefined,
  ): Promise<void> {
    // Each chunk's nodes, by hash, for the build glow.
    const nodesByHash = new Map(
      chunks.map((c, i) => [c.hash, plan.chunks[i]?.nodeIds ?? []]),
    );
    const byNode = (outcomes: ReadonlyMap<string, StepRunOutcome>) => {
      const result = new Map<string, StepRunOutcome>();
      for (const [hash, outcome] of outcomes) {
        for (const nid of nodesByHash.get(hash) ?? []) result.set(nid, outcome);
      }
      return result;
    };

    const result = await runChunks(
      this.engine,
      chunks,
      isOutputMaterialized(index, plan),
      this.cache,
      {
        onProgress: (outcomes) => {
          this._lastRunOutcomes = outcomes;
          this._nodeOutcomes = byNode(outcomes);
          m.redraw();
        },
        onChunkEnd: async (_chunk, queryTimeMs) => {
          if (this.latestUpdate?.emulateSlowLoads && !this.disposed) {
            await new Promise<void>((r) => setTimeout(r, queryTimeMs * 10));
          }
        },
      },
    );

    // Record the run in the cache, even if it failed part-way, so every table
    // that was built is tracked (and eventually dropped).
    const now = performance.now();
    for (const hash of result.hits) {
      const entry = this.cache.get(hash);
      if (entry === undefined) continue;
      entry.lastHitAt = now;
      entry.hitCount++;
    }
    for (const entry of result.built) this.cache.set(entry.hash, {...entry});
    this._lastRunOutcomes = result.outcomes;
    this._nodeOutcomes = byNode(result.outcomes);

    if (this.disposed) return;
    if (result.error !== undefined) throw new Error(result.error);

    // The DataGrid wraps its source in plain SQL, so it reads the output with
    // a Standard SQL query whatever the dialect.
    const read = outputQuery(index, plan, chunks, 'sql', {sqlModules});
    if (!read.ok) throw new Error(read.error);
    const {sql, includes, relation} = read;
    const readQuerySql =
      relation !== undefined ? `SELECT * FROM ${relation}` : sql;
    this._materializedTable =
      relation !== undefined ? relation : `(\n${sql}\n)`;
    this.readHashes = new Set(result.outputReads);

    if (this._readQuerySql !== readQuerySql || !this._dataSource) {
      this._dataSource?.dispose();
      this._readQuerySql = readQuerySql;
      this._dataSource = new SQLDataSource({
        engine: this.engine,
        tableOrSubquery: readQuerySql,
        // The read query's includes run once, before the grid's own queries.
        preamble:
          includes.length > 0
            ? includes
                .map((inc) => `INCLUDE PERFETTO MODULE ${inc};`)
                .join('\n')
            : undefined,
        queue: this.taskQueue,
      });
    }
  }

  // Least-recently-used eviction down to `maxEntries`, never dropping a table
  // the current output query reads. Runs after each run (never mid-run), and
  // re-running a plan only needs its most downstream cached step, so dropping
  // older upstream tables doesn't cause rebuild cascades.
  private async evict(): Promise<void> {
    const excess = this.cache.size - this.maxEntries;
    if (excess <= 0) return;
    const victims = [...this.cache.values()]
      .filter((e) => !this.readHashes.has(e.hash))
      .sort((a, b) => a.lastHitAt - b.lastHitAt || a.createdAt - b.createdAt)
      .slice(0, excess);
    for (const victim of victims) this.cache.delete(victim.hash);
    await this.dropTables(victims.map((v) => v.hash));
  }

  private runOnTaskQueue(fn: () => Promise<void>): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      this.taskQueue.schedule({}, async () => {
        try {
          await fn();
          resolve();
        } catch (e) {
          reject(e);
        }
      });
    });
  }

  private async dropTables(hashes: readonly string[]): Promise<void> {
    if (hashes.length === 0) return;
    await this.runOnTaskQueue(async () => {
      for (const hash of hashes) {
        await this.engine.tryQuery(`DROP TABLE IF EXISTS ${hash}`);
      }
    });
  }

  private async dropAll(): Promise<void> {
    const hashes = Array.from(this.cache.keys());
    this.cache.clear();
    await this.dropTables(hashes);
  }

  async dispose(): Promise<void> {
    if (this.disposed) return;
    this.disposed = true;
    this._dataSource?.dispose();
    this._dataSource = undefined;
    this._readQuerySql = undefined;
    await this.limiter.schedule(async () => {
      await this.dropAll();
    });
  }
}
