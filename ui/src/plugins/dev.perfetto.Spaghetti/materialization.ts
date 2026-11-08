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
import {flattenNodes, fnvHash, type GraphIndex} from './graph_utils';
import {
  buildDisplayQuery,
  compose,
  formatPlan,
  type FormatResult,
} from './compose';
import {buildCanvasPlan, buildPlan, type Plan} from './plan';
import type {SqlDialect} from './sql_types';

const DEFAULT_MAX_ORPHANS = 16;

/** A cache table: an expensive node plus the cheap nodes folded into it. */
export interface MaterializationStep {
  readonly hash: string;
  /** The expensive node whose output this table holds. */
  readonly rootId: string;
  /** Nodes rendered inline in this step's query, in topological order. */
  readonly nodeIds: readonly string[];
  /** Hashes of the cache tables this step reads from. */
  readonly deps: readonly string[];
}

/** The query that reads the target's result (always Standard SQL). */
export interface MaterializationOutput {
  readonly rootId: string;
  /** Nodes rendered inline; empty when the target itself is a cache table. */
  readonly nodeIds: readonly string[];
  readonly deps: readonly string[];
}

export interface MaterializationPlan {
  readonly plan: Plan;
  /** The dialect the steps are built in (part of every step's identity). */
  readonly dialect: SqlDialect;
  /** Cache table hash of every expensive node in the plan. */
  readonly hashes: ReadonlyMap<string, string>;
  /** In dependency order: every step comes after the steps it reads. */
  readonly steps: readonly MaterializationStep[];
  readonly output: MaterializationOutput;
  readonly includes: readonly string[];
}

/** Nodes reachable from `rootId` without crossing an expensive node. */
function collectStepNodes(
  plan: Plan,
  rootId: string,
  rootIsMember: boolean,
): {nodeIds: string[]; deps: string[]} {
  const nodeById = new Map(plan.nodes.map((n) => [n.nodeId, n]));
  const members = new Set<string>();
  const boundary = new Set<string>();
  function visit(id: string): void {
    if (members.has(id) || boundary.has(id)) return;
    const isCut = nodeById.get(id)?.expensive ?? false;
    if (isCut && !(rootIsMember && id === rootId)) {
      boundary.add(id);
      return;
    }
    members.add(id);
    nodeById.get(id)?.inputs.forEach((inp) => visit(inp.nodeId));
  }
  visit(rootId);
  return {
    nodeIds: plan.nodes.map((n) => n.nodeId).filter((id) => members.has(id)),
    deps: [...boundary],
  };
}

/**
 * Cache identity of every expensive node: the hash of the exact query that
 * builds its table, in `dialect`, with the tables it reads named by their own
 * hashes. That makes it a Merkle hash: any change upstream renames a
 * dependency and so changes the text. Different dialects never share tables.
 */
export function computeStepHashes(
  plan: Plan,
  dialect: SqlDialect,
): Map<string, string> {
  const hashes = new Map<string, string>();
  for (const node of plan.nodes) {
    // Plan order is topological, so dependencies are hashed first.
    if (!node.expensive) continue;
    const sub = collectStepNodes(plan, node.nodeId, true);
    const res = compose(
      plan,
      {
        members: new Set(sub.nodeIds),
        rootId: node.nodeId,
        tableFor: (id) => hashes.get(id) ?? id,
      },
      dialect,
    );
    const text = res.ok ? res.sql : `-- error: ${res.error}`;
    hashes.set(node.nodeId, `_qb_${fnvHash(`${dialect}\n${text}`)}`);
  }
  return hashes;
}

// Partition a plan into cacheable steps plus an output query. The output of
// every expensive node becomes a cache table; everything else folds into the
// next table's query or the output query. Cut points are a pure function of
// the plan - the cache only decides which steps execute.
export function planMaterialization(
  plan: Plan,
  dialect: SqlDialect,
): MaterializationPlan {
  const hashes = computeStepHashes(plan, dialect);
  const hashOf = (id: string) => hashes.get(id)!;

  const byHash = new Map<string, MaterializationStep>();
  const orderedHashes: string[] = [];
  const visited = new Set<string>();

  function collectStep(cutId: string): void {
    if (visited.has(cutId)) return;
    visited.add(cutId);
    const hash = hashOf(cutId);
    const sub = collectStepNodes(plan, cutId, true);
    sub.deps.forEach(collectStep);
    const prev = byHash.get(hash);
    if (prev !== undefined) {
      // Identical subgraph elsewhere: same table, credit both sets of nodes.
      byHash.set(hash, {
        ...prev,
        nodeIds: [...new Set([...prev.nodeIds, ...sub.nodeIds])],
      });
      return;
    }
    byHash.set(hash, {
      hash,
      rootId: cutId,
      nodeIds: sub.nodeIds,
      deps: [...new Set(sub.deps.map(hashOf))],
    });
    orderedHashes.push(hash);
  }

  const out = collectStepNodes(plan, plan.targetNodeId, false);
  out.deps.forEach(collectStep);

  return {
    plan,
    dialect,
    hashes,
    steps: orderedHashes.map((h) => byHash.get(h)!),
    output: {
      rootId: plan.targetNodeId,
      nodeIds: out.nodeIds,
      deps: [...new Set(out.deps.map(hashOf))],
    },
    includes: plan.includes,
  };
}

/**
 * Hashes of every cache table the canvas currently uses (every valid
 * expensive node, in `dialect`). These must not be evicted.
 */
export function collectLiveHashes(
  index: GraphIndex,
  sqlModules: SqlModules | undefined,
  dialect: SqlDialect,
): Set<string> {
  return new Set(
    computeStepHashes(buildCanvasPlan(index, sqlModules), dialect).values(),
  );
}

function hashToNodeTable(
  matPlan: MaterializationPlan,
  tableName: (hash: string) => string,
): (nodeId: string) => string {
  return (nodeId) => tableName(matPlan.hashes.get(nodeId) ?? nodeId);
}

/**
 * The query body that builds a step's cache table (without the
 * `CREATE PERFETTO TABLE` wrapper), in the plan's dialect. `tableName` maps a
 * dependency's hash to the table to read it from (default: the hash itself,
 * which is exactly the text the step's hash was computed from).
 */
export function formatStep(
  matPlan: MaterializationPlan,
  step: MaterializationStep,
  tableName: (hash: string) => string = (h) => h,
): FormatResult {
  return compose(
    matPlan.plan,
    {
      members: new Set(step.nodeIds),
      rootId: step.rootId,
      tableFor: hashToNodeTable(matPlan, tableName),
    },
    matPlan.dialect,
  );
}

/**
 * The query that reads the target's result. Always Standard SQL: the DataGrid
 * and details panels wrap it in plain SQL, which can't contain a pipe query.
 */
export function formatOutput(
  matPlan: MaterializationPlan,
  tableName: (hash: string) => string = (h) => h,
): FormatResult {
  const {output} = matPlan;
  if (output.nodeIds.length === 0) {
    const table = hashToNodeTable(matPlan, tableName)(output.rootId);
    return {
      ok: true,
      sql: `SELECT * FROM ${table}`,
      includes: [],
      relation: table,
    };
  }
  return compose(
    matPlan.plan,
    {
      members: new Set(output.nodeIds),
      rootId: output.rootId,
      tableFor: hashToNodeTable(matPlan, tableName),
    },
    'sql',
  );
}

// Return the subset of `matPlan.steps` hashes required to serve the output:
// walks backward from the output and stops descending past already-warm steps
// so evicted ancestors of warm tables are never unnecessarily rebuilt.
export function computeNeededStepHashes(
  matPlan: MaterializationPlan,
  warmHashes: ReadonlySet<string>,
): Set<string> {
  const stepByHash = new Map(matPlan.steps.map((s) => [s.hash, s]));
  const needed = new Set<string>();
  function visit(hash: string): void {
    if (needed.has(hash)) return;
    needed.add(hash);
    if (!warmHashes.has(hash)) {
      stepByHash.get(hash)?.deps.forEach(visit);
    }
  }
  matPlan.output.deps.forEach(visit);
  return needed;
}

export interface MaterializationOptions {
  readonly maxOrphans?: number;
  readonly taskQueue?: AtomicTaskQueue;
}

interface StoredCacheEntry {
  readonly hash: string;
  readonly sql: string;
  readonly createdAt: number;
  readonly materializeTimeMs: number;
  lastHitAt: number;
  hitCount: number;
  priority: number;
}

export interface CacheEntry {
  readonly hash: string;
  readonly sql: string;
  readonly createdAt: number;
  readonly materializeTimeMs: number;
  readonly lastHitAt: number;
  readonly hitCount: number;
  readonly isLive: boolean;
}

export type StepRunOutcome =
  | {readonly kind: 'building'}
  | {readonly kind: 'hit'; readonly savedTimeMs: number}
  | {readonly kind: 'built'; readonly timeMs: number}
  | {readonly kind: 'skipped'};

export class MaterializationService {
  private readonly limiter = new AsyncLimiter();
  private readonly engine: Engine;
  private readonly taskQueue: AtomicTaskQueue;
  private readonly maxOrphans: number;

  private cache = new Map<string, StoredCacheEntry>();
  private evictionClock = 0;

  private lastStore: NodeQueryBuilderStore | undefined;
  private lastSqlModules: SqlModules | undefined;
  private lastDialect: SqlDialect | undefined;
  private lastIndex: GraphIndex = {nodes: {}, childToParent: new Map()};
  private liveHashes: ReadonlySet<string> = new Set();

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

  private _evaluatedNodeId: string | undefined;
  private _displaySql: string | undefined;
  private _activeDialect: SqlDialect = 'sql';
  private _error: string | undefined;
  private _dataSource: SQLDataSource | undefined;
  private _readQuerySql: string | undefined;
  private _materializedTable: string | undefined;

  private _materializingNodeIds = new Set<string>();
  private _fadingOutNodeIds = new Set<string>();
  private fadeTimers = new Map<string, ReturnType<typeof setTimeout>>();
  private _lastRunOutcomes: ReadonlyMap<string, StepRunOutcome> = new Map();
  private _lastIncludesTimeMs: number | undefined;

  emulateSlowLoads = false;
  private disposed = false;

  constructor(engine: Engine, options?: MaterializationOptions) {
    this.engine = engine;
    this.maxOrphans = options?.maxOrphans ?? DEFAULT_MAX_ORPHANS;
    this.taskQueue = options?.taskQueue ?? new AtomicTaskQueue();
  }

  get dataSource(): SQLDataSource | undefined {
    return this._dataSource;
  }

  get evaluatedNodeId(): string | undefined {
    return this._evaluatedNodeId;
  }

  get readQuerySql(): string | undefined {
    return this._readQuerySql;
  }

  get materializedTable(): string | undefined {
    return this._materializedTable;
  }

  get warmHashes(): ReadonlySet<string> {
    return new Set(this.cache.keys());
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

  get cacheEntries(): readonly CacheEntry[] {
    return Array.from(this.cache.values(), (entry) => ({
      hash: entry.hash,
      sql: entry.sql,
      createdAt: entry.createdAt,
      materializeTimeMs: entry.materializeTimeMs,
      lastHitAt: entry.lastHitAt,
      hitCount: entry.hitCount,
      isLive: this.liveHashes.has(entry.hash),
    }));
  }

  get materializingNodeIds(): ReadonlySet<string> {
    return this._materializingNodeIds;
  }

  get fadingOutNodeIds(): ReadonlySet<string> {
    return this._fadingOutNodeIds;
  }

  get lastRunOutcomes(): ReadonlyMap<string, StepRunOutcome> {
    return this._lastRunOutcomes;
  }

  get lastIncludesTimeMs(): number | undefined {
    return this._lastIncludesTimeMs;
  }

  planFor(plan: Plan, dialect: SqlDialect): MaterializationPlan {
    return planMaterialization(plan, dialect);
  }

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

  pruneOrphans(): Promise<void> {
    if (this.disposed) return Promise.resolve();
    this.pendingPruneOrphans = true;
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
        this.pendingPruneOrphans = false;
        this.clearActiveOutput();
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
        if (evicted > 0) m.redraw();
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
    if (
      store !== this.lastStore ||
      sqlModules !== this.lastSqlModules ||
      dialect !== this.lastDialect
    ) {
      this.lastStore = store;
      this.lastSqlModules = sqlModules;
      this.lastDialect = dialect;
      this.lastIndex = flattenNodes(store.nodes);
      this.liveHashes = collectLiveHashes(this.lastIndex, sqlModules, dialect);
    }
    return this.lastIndex;
  }

  private clearActiveOutput(): void {
    this._dataSource?.dispose();
    this._dataSource = undefined;
    this._evaluatedNodeId = undefined;
    this._readQuerySql = undefined;
    this._materializedTable = undefined;
    this._displaySql = undefined;
    this._error = undefined;
    this._lastRunOutcomes = new Map();
    this._lastIncludesTimeMs = undefined;
  }

  private async doUpdate(
    store: NodeQueryBuilderStore,
    activeNodeId: string | undefined,
    sqlModules: SqlModules | undefined,
    dialect: SqlDialect,
  ): Promise<void> {
    const index = this.updateGraphState(store, sqlModules, dialect);
    const plan = activeNodeId
      ? buildPlan(index, activeNodeId, sqlModules)
      : undefined;

    if (!plan) {
      this.clearActiveOutput();
      this._activeDialect = dialect;
      const evicted = await this.evictOrphans(this.maxOrphans);
      if (evicted > 0) m.redraw();
      return;
    }

    this._evaluatedNodeId = activeNodeId;

    const displayRes = formatPlan(plan, dialect, {
      includeModuleHeaders: true,
    });
    const displaySql = buildDisplayQuery(plan, dialect);

    if (
      displaySql !== undefined &&
      displaySql === this._displaySql &&
      dialect === this._activeDialect &&
      (this._dataSource !== undefined || this._error !== undefined)
    ) {
      const evicted = await this.evictOrphans(this.maxOrphans);
      if (evicted > 0) m.redraw();
      return;
    }

    this._displaySql = displaySql;
    this._activeDialect = dialect;

    try {
      if (!displayRes.ok) throw new Error(displayRes.error);
      const matPlan = this.planFor(plan, dialect);
      await this.executeMaterializationPlan(matPlan);
      this._error = undefined;
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      console.error('[QueryBuilder] Materialization error:', msg);
      this._error = msg;
      this._dataSource?.dispose();
      this._dataSource = undefined;
      this._readQuerySql = undefined;
      this._materializedTable = undefined;
      this.clearFadeTimers();
      this._materializingNodeIds.clear();
      this._fadingOutNodeIds.clear();
      this._lastRunOutcomes = new Map();
      this._lastIncludesTimeMs = undefined;
      m.redraw();
    }

    const evicted = await this.evictOrphans(this.maxOrphans);
    if (evicted > 0) m.redraw();
  }

  private computePriority(timeMs: number, hitCount: number): number {
    return this.evictionClock + Math.max(1, hitCount) * Math.max(1, timeMs);
  }

  private async executeMaterializationPlan(
    matPlan: MaterializationPlan,
  ): Promise<void> {
    const neededHashes = computeNeededStepHashes(
      matPlan,
      new Set(this.cache.keys()),
    );
    const formattedSteps: Array<{step: MaterializationStep; sql: string}> = [];
    for (const step of matPlan.steps) {
      if (!neededHashes.has(step.hash)) continue;
      const res = formatStep(matPlan, step);
      if (!res.ok) throw new Error(res.error);
      formattedSteps.push({step, sql: res.sql});
    }

    const finalReadRes = formatOutput(matPlan);
    if (!finalReadRes.ok) throw new Error(finalReadRes.error);

    this._lastIncludesTimeMs = undefined;
    if (matPlan.includes.length > 0) {
      const incStart = performance.now();
      for (const inc of matPlan.includes) {
        await this.engine.query(`INCLUDE PERFETTO MODULE ${inc}`);
      }
      this._lastIncludesTimeMs = performance.now() - incStart;
    }

    const runOutcomes = new Map<string, StepRunOutcome>();
    for (const step of matPlan.steps) {
      if (!neededHashes.has(step.hash)) {
        runOutcomes.set(step.hash, {kind: 'skipped'});
      }
    }
    this._lastRunOutcomes = runOutcomes;

    for (const {step, sql} of formattedSteps) {
      const cached = this.cache.get(step.hash);
      if (cached) {
        cached.lastHitAt = performance.now();
        cached.hitCount++;
        cached.priority = this.computePriority(
          cached.materializeTimeMs,
          cached.hitCount,
        );
        runOutcomes.set(step.hash, {
          kind: 'hit',
          savedTimeMs: cached.materializeTimeMs,
        });
        continue;
      }

      runOutcomes.set(step.hash, {kind: 'building'});
      for (const nid of step.nodeIds) {
        const prevTimer = this.fadeTimers.get(nid);
        if (prevTimer !== undefined) {
          clearTimeout(prevTimer);
          this.fadeTimers.delete(nid);
        }
        this._fadingOutNodeIds.delete(nid);
        this._materializingNodeIds.add(nid);
      }
      m.redraw();

      const createSql = `CREATE PERFETTO TABLE ${step.hash} AS\n${sql}`;
      console.log(
        `[QueryBuilder] Materializing ${step.hash}:\n  ${createSql.replace(/\n/g, '\n  ')}`,
      );

      const start = performance.now();
      await this.engine.query(
        `DROP TABLE IF EXISTS ${step.hash};\n${createSql}`,
      );
      const queryTimeMs = performance.now() - start;
      if (this.emulateSlowLoads && !this.disposed) {
        await new Promise<void>((r) => setTimeout(r, queryTimeMs * 10));
      }
      if (this.disposed) return;
      const timeMs = performance.now() - start;
      const now = performance.now();

      for (const nid of step.nodeIds) {
        this._materializingNodeIds.delete(nid);
        this._fadingOutNodeIds.add(nid);
        const prevTimer = this.fadeTimers.get(nid);
        if (prevTimer !== undefined) clearTimeout(prevTimer);
        this.fadeTimers.set(
          nid,
          setTimeout(() => {
            this.fadeTimers.delete(nid);
            this._fadingOutNodeIds.delete(nid);
            m.redraw();
          }, 900),
        );
      }

      this.cache.set(step.hash, {
        hash: step.hash,
        sql,
        createdAt: now,
        materializeTimeMs: timeMs,
        lastHitAt: now,
        hitCount: 1,
        priority: this.computePriority(timeMs, 1),
      });
      runOutcomes.set(step.hash, {kind: 'built', timeMs});
    }

    const singleScan = finalReadRes.relation;

    const readQuerySql =
      singleScan !== undefined
        ? `SELECT * FROM ${singleScan}`
        : finalReadRes.sql;
    this._materializedTable =
      singleScan !== undefined ? singleScan : `(\n${finalReadRes.sql}\n)`;

    if (this._readQuerySql !== readQuerySql || !this._dataSource) {
      this._dataSource?.dispose();
      this._readQuerySql = readQuerySql;
      this._dataSource = new SQLDataSource({
        engine: this.engine,
        tableOrSubquery: readQuerySql,
        queue: this.taskQueue,
      });
    }
    m.redraw();
  }

  private isProtected(hash: string): boolean {
    return this.liveHashes.has(hash) || hash === this._materializedTable;
  }

  private async evictOrphans(maxOrphans: number): Promise<number> {
    const orphans: StoredCacheEntry[] = [];
    for (const entry of this.cache.values()) {
      if (!this.isProtected(entry.hash)) orphans.push(entry);
    }
    if (orphans.length <= maxOrphans) return 0;

    orphans.sort(
      (a, b) =>
        a.priority - b.priority ||
        a.lastHitAt - b.lastHitAt ||
        a.createdAt - b.createdAt,
    );

    const victims = orphans.slice(0, orphans.length - maxOrphans);
    for (const victim of victims) {
      this.evictionClock = Math.max(this.evictionClock, victim.priority);
      this.cache.delete(victim.hash);
    }
    const hashes = victims.map((v) => v.hash);
    await this.dropTables(hashes);
    return hashes.length;
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
    this.evictionClock = 0;
    await this.dropTables(hashes);
  }

  private clearFadeTimers(): void {
    for (const timer of this.fadeTimers.values()) clearTimeout(timer);
    this.fadeTimers.clear();
  }

  async dispose(): Promise<void> {
    if (this.disposed) return;
    this.disposed = true;
    this.clearFadeTimers();
    this._dataSource?.dispose();
    this._dataSource = undefined;
    this._readQuerySql = undefined;
    await this.limiter.schedule(async () => {
      await this.dropAll();
    });
  }
}
