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
import {SerialTaskQueue} from '../../base/query_slot';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import {createSimpleSchema} from '../../components/widgets/datagrid/sql_schema';
import {Engine} from '../../trace_processor/engine';
import {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import {buildDisplaySql, buildIR, IrEntry} from './ir';
import {NodeQueryBuilderStore} from './graph_model';

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

// Per-entry metadata in the cache.
export interface CacheEntry {
  readonly hash: string;
  readonly sql: string;
  readonly createdAt: number; // performance.now() timestamp
  readonly materializeTimeMs: number;
  lastHitAt: number; // performance.now() timestamp of last cache hit
  hitCount: number;
}

// Materialization service for the query builder.
//
// Uses the content-addressable IR where the hash IS the table name.
// Tables are keyed by hash, so clicking between nodes reuses existing
// materialized tables. No SQL rewriting needed — the IR's SQL already
// references deps by their hash.
//
// All work is serialized through a single AsyncLimiter.
export class MaterializationService {
  private readonly limiter = new AsyncLimiter();
  private readonly engine: Engine;
  private readonly taskQueue = new SerialTaskQueue();

  // Cache: hash -> metadata (hash IS the table name).
  private cache = new Map<string, CacheEntry>();

  // The display SQL for the user to see.
  private _displaySql: string | undefined;

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

  constructor(engine: Engine) {
    this.engine = engine;
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

  get error(): string | undefined {
    return this._error;
  }

  get queryReport(): QueryReport | undefined {
    return this._queryReport;
  }

  get cacheEntries(): readonly CacheEntry[] {
    return Array.from(this.cache.values());
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
  ): void {
    if (this.disposed) return;

    this.limiter.schedule(async () => {
      if (this.disposed) return;
      await this.doUpdate(store, activeNodeId, sqlModules);
    });
  }

  private async doUpdate(
    store: NodeQueryBuilderStore,
    activeNodeId: string | undefined,
    sqlModules: SqlModules | undefined,
  ): Promise<void> {
    if (!activeNodeId) {
      this._dataSource?.dispose();
      this._dataSource = undefined;
      this._materializedTable = undefined;
      this._displaySql = undefined;
      this._error = undefined;
      return;
    }

    const entries = buildIR(
      store.nodes,
      store.connections,
      activeNodeId,
      sqlModules,
    );
    if (!entries || entries.length === 0) {
      this._dataSource?.dispose();
      this._dataSource = undefined;
      this._materializedTable = undefined;
      this._displaySql = undefined;
      this._error = undefined;
      return;
    }

    const displaySql = buildDisplaySql(entries);
    if (!displaySql) {
      this._dataSource?.dispose();
      this._dataSource = undefined;
      this._materializedTable = undefined;
      this._displaySql = undefined;
      this._error = undefined;
      return;
    }

    // Nothing changed — skip entirely.
    if (displaySql === this._displaySql) {
      return;
    }

    this._displaySql = displaySql;

    try {
      await this.materializeEntries(entries);
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
  }

  private async materializeEntries(entries: IrEntry[]): Promise<void> {
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

      const createSql = `CREATE TABLE ${entry.hash} AS ${entry.sql}`;

      console.log(
        `[QueryBuilder] Materializing ${entry.hash}:\n  ${createSql.replace(/\n/g, '\n  ')}`,
      );

      const start = performance.now();
      await this.engine.query(
        `DROP TABLE IF EXISTS ${entry.hash}; ${createSql}`,
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
    const finalSql = `SELECT * FROM ${lastEntry.hash}`;

    this._materializedTable = lastEntry.hash;
    this._dataSource?.dispose();
    this._dataSource = new SQLDataSource({
      engine: this.engine,
      sqlSchema: createSimpleSchema(finalSql),
      rootSchemaName: 'query',
      queue: this.taskQueue,
    });
  }

  private async dropAll(): Promise<void> {
    for (const [hash] of this.cache) {
      await this.engine.tryQuery(`DROP TABLE IF EXISTS ${hash}`);
    }
    this.cache.clear();
  }

  // Clear the cache and force re-materialization on next update.
  clearCache(): void {
    this.limiter.schedule(async () => {
      if (this.disposed) return;
      await this.dropAll();
      this._dataSource?.dispose();
      this._dataSource = undefined;
      this._materializedTable = undefined;
      this._displaySql = undefined;
      this._queryReport = undefined;
    });
  }

  async dispose(): Promise<void> {
    if (this.disposed) return;
    this.disposed = true;
    this._dataSource?.dispose();
    await this.dropAll();
  }
}
