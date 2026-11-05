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

import m from 'mithril';
import {AsyncLimiter} from '../../base/async_limiter';
import {AtomicTaskQueue} from '../../base/async_memo';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import type {Engine} from '../../trace_processor/engine';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import type {NodeQueryBuilderStore} from './graph_model';
import {flattenNodes, type GraphIndex} from './graph_utils';
import {
  buildDisplayQuery,
  buildIR,
  collectLiveHashes,
  type IrEntry,
  type SqlDialect,
} from './ir';

// Default cap on stale/orphaned tables retained for undo & mid-edit recovery.
// Live tables (belonging to any valid node currently in the graph) are never
// evicted regardless of graph size.
const DEFAULT_MAX_ORPHANS = 16;

export interface MaterializationOptions {
  readonly maxOrphans?: number;
  readonly taskQueue?: AtomicTaskQueue;
}

// Per-entry report from the most recent materialization cycle.
export interface EntryReport {
  readonly hash: string;
  readonly sql: string;
  readonly cacheHit: boolean;
  // Time in ms to materialize (0 for cache hits).
  readonly timeMs: number;
}

// Report for the most recent materialization cycle.
export interface QueryReport {
  readonly entries: readonly EntryReport[];
  readonly totalTimeMs: number;
}

// Internal mutable cache record.
interface StoredCacheEntry {
  readonly hash: string;
  readonly sql: string;
  readonly createdAt: number; // performance.now() timestamp
  readonly materializeTimeMs: number;
  lastHitAt: number; // performance.now() timestamp of last cache hit
  hitCount: number;
}

// Per-entry metadata in the cache.
export interface CacheEntry {
  readonly hash: string;
  readonly sql: string;
  readonly createdAt: number; // performance.now() timestamp
  readonly materializeTimeMs: number;
  readonly lastHitAt: number; // performance.now() timestamp of last cache hit
  readonly hitCount: number;
  // True when at least one valid node currently in the graph emits this hash.
  readonly isLive: boolean;
}

// Materialization service for the query builder.
//
// Uses the content-addressable IR where the hash IS the table name.
// Tables are keyed by hash, so clicking between nodes reuses existing
// materialized tables. No SQL rewriting needed — the IR's SQL already
// references deps by their hash.
//
// Cache management uses a two-pool policy:
// 1. Live hashes (reachable from any valid node currently in the graph) and
//    the active materialized table are pinned and never evicted.
// 2. Orphaned hashes (left behind by edits or node deletions) are bounded by
//    a small LRU cap (`maxOrphans`), with `DROP TABLE` serialized on the same
//    `AtomicTaskQueue` as `SQLDataSource` so drops never race DataGrid reads.
//
// All work is serialized through a single AsyncLimiter.
export class MaterializationService {
  private readonly limiter = new AsyncLimiter();
  private readonly engine: Engine;
  private readonly taskQueue: AtomicTaskQueue;
  private readonly maxOrphans: number;

  // Cache: hash -> metadata (hash IS the table name).
  private cache = new Map<string, StoredCacheEntry>();

  // Memoized graph flatten + live hash set keyed by store, sqlModules & dialect.
  private lastStore: NodeQueryBuilderStore | undefined;
  private lastSqlModules: SqlModules | undefined;
  private lastDialect: SqlDialect | undefined;
  private lastIndex: GraphIndex = {nodes: {}, childToParent: new Map()};
  private liveHashes: ReadonlySet<string> = new Set();

  // Coalesced pending work flags so AsyncLimiter never drops a clear/prune
  // when a Mithril redraw immediately schedules another update behind it.
  private latestUpdate:
    | {
        readonly store: NodeQueryBuilderStore;
        readonly activeNodeId: string | undefined;
        readonly sqlModules: SqlModules | undefined;
        readonly dialect: SqlDialect;
      }
    | undefined;
  private pendingUpdate = false;
  private pendingClearAll = false;
  private pendingPruneOrphans = false;

  // The display query for the active materialization dialect.
  private _displaySql: string | undefined;
  private _activeDialect: SqlDialect = 'sql';

  // Error from the last materialization attempt.
  private _error: string | undefined;

  // The SQLDataSource for the DataGrid.
  private _dataSource: SQLDataSource | undefined;

  // The table name of the final materialized IR entry ('_qb_<hash>').
  private _materializedTable: string | undefined;

  // Report from the most recent materialization cycle.
  private _queryReport: QueryReport | undefined;

  // Node IDs currently being materialized.
  private _materializingNodeIds = new Set<string>();

  // Node IDs whose pulse is fading out after materialization completed.
  private _fadingOutNodeIds = new Set<string>();

  private disposed = false;

  constructor(engine: Engine, options?: MaterializationOptions) {
    this.engine = engine;
    this.maxOrphans = options?.maxOrphans ?? DEFAULT_MAX_ORPHANS;
    this.taskQueue = options?.taskQueue ?? new AtomicTaskQueue();
  }

  get dataSource(): SQLDataSource | undefined {
    return this._dataSource;
  }

  get materializedTable(): string | undefined {
    return this._materializedTable;
  }

  get displaySql(): string | undefined {
    return this._displaySql;
  }

  get activeDialect(): SqlDialect {
    return this._activeDialect;
  }

  get error(): string | undefined {
    return this._error;
  }

  get queryReport(): QueryReport | undefined {
    return this._queryReport;
  }

  get cacheEntries(): readonly CacheEntry[] {
    return Array.from(this.cache.values(), (entry) => ({
      ...entry,
      isLive: this.liveHashes.has(entry.hash),
    }));
  }

  get materializingNodeIds(): ReadonlySet<string> {
    return this._materializingNodeIds;
  }

  get fadingOutNodeIds(): ReadonlySet<string> {
    return this._fadingOutNodeIds;
  }

  // Schedule a materialization update. Call this whenever the graph changes.
  scheduleUpdate(
    store: NodeQueryBuilderStore,
    activeNodeId: string | undefined,
    sqlModules: SqlModules | undefined,
    dialect: SqlDialect = 'sql',
  ): Promise<void> {
    if (this.disposed) return Promise.resolve();
    this.updateGraphState(store, sqlModules, dialect);
    this.latestUpdate = {store, activeNodeId, sqlModules, dialect};
    this.pendingUpdate = true;
    return this.scheduleWork();
  }

  // Drop all orphaned cache entries immediately while preserving live tables.
  pruneOrphans(): Promise<void> {
    if (this.disposed) return Promise.resolve();
    this.pendingPruneOrphans = true;
    return this.scheduleWork();
  }

  // Clear the cache and force re-materialization on next update.
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
        this.pendingPruneOrphans = false;
        this.clearActiveOutput();
        this._queryReport = undefined;
        await this.dropAll();
        m.redraw();
      } else if (this.pendingPruneOrphans) {
        this.pendingPruneOrphans = false;
        if (this.latestUpdate) {
          this.updateGraphState(
            this.latestUpdate.store,
            this.latestUpdate.sqlModules,
            this.latestUpdate.dialect,
          );
        }
        const evicted = await this.evictOrphans(0);
        if (evicted > 0) {
          m.redraw();
        }
      }

      if (this.disposed || !this.pendingUpdate || !this.latestUpdate) return;
      this.pendingUpdate = false;
      const {store, activeNodeId, sqlModules, dialect} = this.latestUpdate;
      await this.doUpdate(store, activeNodeId, sqlModules, dialect);
    });
  }

  private updateGraphState(
    store: NodeQueryBuilderStore,
    sqlModules: SqlModules | undefined,
    dialect: SqlDialect,
  ): GraphIndex {
    const graphChanged =
      store !== this.lastStore || sqlModules !== this.lastSqlModules;
    if (graphChanged) {
      this.lastStore = store;
      this.lastSqlModules = sqlModules;
      this.lastIndex = flattenNodes(store.nodes);
    }
    if (graphChanged || dialect !== this.lastDialect) {
      this.lastDialect = dialect;
      this.liveHashes = collectLiveHashes(this.lastIndex, sqlModules, dialect);
    }
    return this.lastIndex;
  }

  private clearActiveOutput(): void {
    this._dataSource?.dispose();
    this._dataSource = undefined;
    this._materializedTable = undefined;
    this._displaySql = undefined;
    this._error = undefined;
  }

  private async doUpdate(
    store: NodeQueryBuilderStore,
    activeNodeId: string | undefined,
    sqlModules: SqlModules | undefined,
    dialect: SqlDialect,
  ): Promise<void> {
    const index = this.updateGraphState(store, sqlModules, dialect);

    if (!activeNodeId) {
      this.clearActiveOutput();
      this._activeDialect = dialect;
      const evicted = await this.evictOrphans(this.maxOrphans);
      if (evicted > 0) m.redraw();
      return;
    }

    const entries = buildIR(index, activeNodeId, sqlModules, dialect);
    if (!entries || entries.length === 0) {
      this.clearActiveOutput();
      this._activeDialect = dialect;
      const evicted = await this.evictOrphans(this.maxOrphans);
      if (evicted > 0) m.redraw();
      return;
    }

    const displaySql = buildDisplayQuery(entries, dialect);
    if (!displaySql) {
      this.clearActiveOutput();
      this._activeDialect = dialect;
      const evicted = await this.evictOrphans(this.maxOrphans);
      if (evicted > 0) m.redraw();
      return;
    }

    // Active node's query and dialect didn't change, though other branches may
    // have been edited or deleted — still enforce the orphan cap.
    if (displaySql === this._displaySql && dialect === this._activeDialect) {
      const evicted = await this.evictOrphans(this.maxOrphans);
      if (evicted > 0) m.redraw();
      return;
    }

    this._displaySql = displaySql;
    this._activeDialect = dialect;

    try {
      await this.materializeEntries(entries, dialect);
      this._error = undefined;
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      console.error('[QueryBuilder] Materialization error:', msg);
      this._error = msg;
      this._dataSource?.dispose();
      this._dataSource = undefined;
      this._materializedTable = undefined;
      // On error, clear all indicators immediately.
      this._materializingNodeIds.clear();
      this._fadingOutNodeIds.clear();
      m.redraw();
    }

    const evicted = await this.evictOrphans(this.maxOrphans);
    if (evicted > 0) m.redraw();
  }

  private async materializeEntries(
    entries: IrEntry[],
    dialect: SqlDialect,
  ): Promise<void> {
    // Ensure required modules are loaded.
    const allIncludes = new Set<string>();
    for (const e of entries) {
      for (const inc of e.includes) allIncludes.add(inc);
    }
    for (const inc of allIncludes) {
      await this.engine.query(`INCLUDE PERFETTO MODULE ${inc}`);
    }

    const reportEntries: EntryReport[] = [];
    const totalStart = performance.now();

    // Materialize all entries. The hash IS the table name.
    for (const entry of entries) {
      const cached = this.cache.get(entry.hash);
      if (cached) {
        cached.lastHitAt = performance.now();
        cached.hitCount++;
        reportEntries.push({
          hash: entry.hash,
          sql: entry.sql,
          cacheHit: true,
          timeMs: 0,
        });
        continue;
      }

      // Mark nodes as materializing and trigger a redraw.
      for (const nid of entry.nodeIds) {
        this._materializingNodeIds.add(nid);
      }
      m.redraw();

      // Artificial delay for testing the loading indicator.
      // await new Promise((r) => setTimeout(r, 500));

      // PerfettoSQL Next pipelines must be materialized via `CREATE PERFETTO TABLE`
      // (`CREATE TABLE t AS FROM ...` is rejected by the PerfettoSQL parser).
      const createSql =
        dialect === 'pipe'
          ? `CREATE PERFETTO TABLE ${entry.hash} AS\n${entry.sql}`
          : `CREATE TABLE ${entry.hash} AS ${entry.sql}`;

      console.log(
        `[QueryBuilder] Materializing ${entry.hash}:\n  ${createSql.replace(/\n/g, '\n  ')}`,
      );

      const start = performance.now();
      await this.engine.query(
        `DROP TABLE IF EXISTS ${entry.hash};\n${createSql}`,
      );
      const timeMs = performance.now() - start;
      const now = performance.now();

      // Move nodes from materializing to fading-out state.
      for (const nid of entry.nodeIds) {
        this._materializingNodeIds.delete(nid);
        this._fadingOutNodeIds.add(nid);
      }
      // Clear fading state after the CSS fade-out animation completes.
      const fadingIds = [...entry.nodeIds];
      setTimeout(() => {
        for (const nid of fadingIds) {
          this._fadingOutNodeIds.delete(nid);
        }
        m.redraw();
      }, 600);

      this.cache.set(entry.hash, {
        hash: entry.hash,
        sql: entry.sql,
        createdAt: now,
        materializeTimeMs: timeMs,
        lastHitAt: now,
        hitCount: 0,
      });
      reportEntries.push({
        hash: entry.hash,
        sql: entry.sql,
        cacheHit: false,
        timeMs,
      });
    }

    this._queryReport = {
      entries: reportEntries,
      totalTimeMs: performance.now() - totalStart,
    };

    // DataGrid reads from the final materialized table.
    const lastEntry = entries[entries.length - 1];
    if (!lastEntry) return;
    const finalSql = `SELECT * FROM ${lastEntry.hash}`;

    this._materializedTable = lastEntry.hash;
    this._dataSource?.dispose();
    this._dataSource = new SQLDataSource({
      engine: this.engine,
      tableOrSubquery: finalSql,
      queue: this.taskQueue,
    });
  }

  private isProtected(hash: string): boolean {
    return this.liveHashes.has(hash) || hash === this._materializedTable;
  }

  private async evictOrphans(maxOrphans: number): Promise<number> {
    const orphans: StoredCacheEntry[] = [];
    for (const entry of this.cache.values()) {
      if (!this.isProtected(entry.hash)) {
        orphans.push(entry);
      }
    }

    if (orphans.length <= maxOrphans) {
      return 0;
    }

    // Evict least-recently-hit orphans first; break ties by creation time.
    orphans.sort(
      (a, b) => a.lastHitAt - b.lastHitAt || a.createdAt - b.createdAt,
    );

    const victims = orphans.slice(0, orphans.length - maxOrphans);
    for (const victim of victims) {
      this.cache.delete(victim.hash);
    }
    const hashes = victims.map((v) => v.hash);
    await this.dropTables(hashes);
    return hashes.length;
  }

  // Run table drops through `taskQueue` so any in-flight `SQLDataSource`
  // query finishes before SQLite drops the underlying table.
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
    await this.limiter.schedule(async () => {
      await this.dropAll();
    });
  }
}
