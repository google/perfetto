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
  analyzeCanvas,
  buildDisplayQuery,
  buildLogicalPlan,
  collectLogicalPlanIncludes,
  formatLogicalPlan,
  formatSql,
  getLogicalOpInputs,
  type LogicalOp,
  type LogicalPlan,
  type SqlDialect,
} from './ir';

const DEFAULT_MAX_ORPHANS = 16;

const ZERO_COST_KINDS: ReadonlySet<LogicalOp['kind']> = new Set([
  'scan',
  'passthrough',
  'as',
  'time_range',
  'data_table',
  'rename',
  'drop',
]);

const PIPELINE_BREAKER_KINDS: ReadonlySet<LogicalOp['kind']> = new Set([
  'aggregate',
  'join',
  'union',
  'interval_intersect',
  'interval_flatten',
  'interval_fill_gaps',
  'interval_merge',
  'intervals_from_events',
  'tree_accumulate',
  'graph_tree',
  'raw_sql',
]);

function isZeroCostOp(op: LogicalOp): boolean {
  return (
    ZERO_COST_KINDS.has(op.kind) ||
    (op.kind === 'project' && op.expressions.length === 0)
  );
}

export interface MaterializationStep {
  readonly hash: string;
  readonly plan: LogicalPlan;
  readonly deps: readonly string[];
  readonly nodeIds: readonly string[];
}

export interface MaterializationPlan {
  readonly steps: readonly MaterializationStep[];
  readonly finalRead: LogicalPlan;
  readonly includes: readonly string[];
}

export interface PlanMaterializationOptions {
  readonly canvasRefCounts?: ReadonlyMap<string, number>;
  readonly warmHashes?: ReadonlySet<string>;
}

// Partition a `LogicalPlan` into dialect-agnostic cacheable materialization
// steps plus a lightweight `finalRead` subplan.
export function planMaterialization(
  plan: LogicalPlan,
  options?: PlanMaterializationOptions,
): MaterializationPlan {
  const {canvasRefCounts, warmHashes} = options ?? {};
  const nodeById = new Map(plan.nodes.map((n) => [n.nodeId, n]));

  const mustCutInputIds = new Set<string>();
  for (const {op} of plan.nodes) {
    if (op.kind === 'union' || op.kind === 'raw_sql') {
      for (const inpId of getLogicalOpInputs(op)) {
        if (nodeById.get(inpId)?.op.kind !== 'scan') {
          mustCutInputIds.add(inpId);
        }
      }
    }
  }

  let targetId = plan.targetNodeId;
  while (true) {
    const t = nodeById.get(targetId);
    if (t?.op.kind === 'passthrough') {
      targetId = t.op.input;
    } else {
      break;
    }
  }

  const cutNodeIds = new Set<string>();
  const zeroCost = new Map<string, boolean>();
  for (const n of plan.nodes) {
    const inputsFree = getLogicalOpInputs(n.op).every(
      (id) => cutNodeIds.has(id) || (zeroCost.get(id) ?? true),
    );
    const isFree = isZeroCostOp(n.op) && inputsFree;
    zeroCost.set(n.nodeId, isFree);

    if (n.op.kind === 'scan' || n.op.kind === 'passthrough') continue;
    const isWarm = warmHashes?.has(n.hash) ?? false;
    if (n.nodeId === targetId) {
      if (isWarm || !isFree) cutNodeIds.add(n.nodeId);
    } else {
      const refs = Math.max(n.refCount, canvasRefCounts?.get(n.nodeId) ?? 0);
      if (
        isWarm ||
        mustCutInputIds.has(n.nodeId) ||
        (refs > 1 && !isFree) ||
        PIPELINE_BREAKER_KINDS.has(n.op.kind)
      ) {
        cutNodeIds.add(n.nodeId);
      }
    }
  }

  function buildSubplan(rootId: string, treatRootAsCut: boolean) {
    const reached = new Set<string>();
    const cutBoundary = new Set<string>();
    function mark(id: string): void {
      if (reached.has(id)) return;
      reached.add(id);
      if ((treatRootAsCut || id !== rootId) && cutNodeIds.has(id)) {
        cutBoundary.add(id);
        return;
      }
      const n = nodeById.get(id);
      if (n) getLogicalOpInputs(n.op).forEach(mark);
    }
    mark(rootId);

    const raw = plan.nodes
      .filter((n) => reached.has(n.nodeId))
      .map((n) =>
        cutBoundary.has(n.nodeId)
          ? {
              nodeId: n.nodeId,
              op: {kind: 'scan' as const, table: n.hash},
              hash: n.hash,
            }
          : {nodeId: n.nodeId, op: n.op, hash: n.hash},
      );
    const refs = new Map<string, number>();
    for (const r of raw) {
      for (const inp of getLogicalOpInputs(r.op)) {
        refs.set(inp, (refs.get(inp) ?? 0) + 1);
      }
    }
    return {
      plan: {
        nodes: raw.map((r) => ({...r, refCount: refs.get(r.nodeId) ?? 0})),
        targetNodeId: rootId,
      },
      cutDeps: [...cutBoundary],
      nodeIds: raw
        .filter((r) => !cutBoundary.has(r.nodeId) && r.op.kind !== 'scan')
        .map((r) => r.nodeId),
    };
  }

  const finalSub = buildSubplan(targetId, true);
  const visited = new Set<string>();
  const byHash = new Map<string, MaterializationStep>();
  const orderedHashes: string[] = [];

  function collectCutStep(cutId: string): void {
    if (visited.has(cutId)) return;
    visited.add(cutId);
    const hash = nodeById.get(cutId)!.hash;
    const sub = buildSubplan(cutId, false);
    sub.cutDeps.forEach(collectCutStep);
    const prev = byHash.get(hash);
    if (prev) {
      byHash.set(hash, {
        ...prev,
        nodeIds: [...new Set([...prev.nodeIds, ...sub.nodeIds])],
      });
      return;
    }
    const deps = [...new Set(sub.cutDeps.map((id) => nodeById.get(id)!.hash))];
    byHash.set(hash, {hash, plan: sub.plan, deps, nodeIds: sub.nodeIds});
    orderedHashes.push(hash);
  }

  finalSub.cutDeps.forEach(collectCutStep);

  return {
    steps: orderedHashes.map((h) => byHash.get(h)!),
    finalRead: finalSub.plan,
    includes: collectLogicalPlanIncludes(plan),
  };
}

// Return the subset of `matPlan.steps` hashes required to serve `finalRead`:
// walks backward from `finalRead` and stops descending past already-warm steps
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
  for (const {op} of matPlan.finalRead.nodes) {
    if (op.kind === 'scan' && stepByHash.has(op.table)) {
      visit(op.table);
    }
  }
  return needed;
}

export interface MaterializationOptions {
  readonly maxOrphans?: number;
  readonly taskQueue?: AtomicTaskQueue;
}

interface StoredCacheEntry {
  readonly hash: string;
  sql: string;
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
  | {readonly kind: 'hit'}
  | {readonly kind: 'built'; readonly timeMs: number};

export class MaterializationService {
  private readonly limiter = new AsyncLimiter();
  private readonly engine: Engine;
  private readonly taskQueue: AtomicTaskQueue;
  private readonly maxOrphans: number;

  private cache = new Map<string, StoredCacheEntry>();
  private evictionClock = 0;

  private lastStore: NodeQueryBuilderStore | undefined;
  private lastSqlModules: SqlModules | undefined;
  private lastIndex: GraphIndex = {nodes: {}, childToParent: new Map()};
  private _canvasRefCounts: ReadonlyMap<string, number> = new Map();
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

  planFor(logicalPlan: LogicalPlan): MaterializationPlan {
    return planMaterialization(logicalPlan, {
      canvasRefCounts: this._canvasRefCounts,
      warmHashes: new Set(this.cache.keys()),
    });
  }

  scheduleUpdate(
    store: NodeQueryBuilderStore,
    activeNodeId: string | undefined,
    sqlModules: SqlModules | undefined,
    dialect: SqlDialect = 'sql',
  ): Promise<void> {
    if (this.disposed) return Promise.resolve();
    this.updateGraphState(store, sqlModules);
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
  ): GraphIndex {
    if (store !== this.lastStore || sqlModules !== this.lastSqlModules) {
      this.lastStore = store;
      this.lastSqlModules = sqlModules;
      this.lastIndex = flattenNodes(store.nodes);
      const analysis = analyzeCanvas(this.lastIndex, sqlModules);
      this._canvasRefCounts = analysis.canvasRefCounts;
      this.liveHashes = analysis.liveHashes;
    }
    return this.lastIndex;
  }

  private clearActiveOutput(): void {
    this._dataSource?.dispose();
    this._dataSource = undefined;
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
    const index = this.updateGraphState(store, sqlModules);
    const logicalPlan = activeNodeId
      ? buildLogicalPlan(index, activeNodeId, sqlModules)
      : undefined;

    if (!logicalPlan) {
      this.clearActiveOutput();
      this._activeDialect = dialect;
      const evicted = await this.evictOrphans(this.maxOrphans);
      if (evicted > 0) m.redraw();
      return;
    }

    const displayRes = formatLogicalPlan(logicalPlan, dialect, {
      includeModuleHeaders: true,
    });
    const displaySql = buildDisplayQuery(logicalPlan, dialect);

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
      const matPlan = this.planFor(logicalPlan);
      await this.executeMaterializationPlan(matPlan, dialect);
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
    dialect: SqlDialect,
  ): Promise<void> {
    const neededHashes = computeNeededStepHashes(
      matPlan,
      new Set(this.cache.keys()),
    );
    const formattedSteps: Array<{step: MaterializationStep; sql: string}> = [];
    for (const step of matPlan.steps) {
      if (!neededHashes.has(step.hash)) continue;
      const res = formatLogicalPlan(step.plan, dialect, {
        includeModuleHeaders: false,
      });
      if (!res.ok) throw new Error(res.error);
      formattedSteps.push({step, sql: res.sql});
    }

    const finalReadRes = formatSql(matPlan.finalRead, {
      includeModuleHeaders: false,
    });
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
    this._lastRunOutcomes = runOutcomes;

    for (const {step, sql} of formattedSteps) {
      const cached = this.cache.get(step.hash);
      if (cached) {
        cached.lastHitAt = performance.now();
        cached.hitCount++;
        cached.sql = sql;
        cached.priority = this.computePriority(
          cached.materializeTimeMs,
          cached.hitCount,
        );
        runOutcomes.set(step.hash, {kind: 'hit'});
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

    const singleScan =
      matPlan.finalRead.nodes.length === 1 &&
      matPlan.finalRead.nodes[0]?.op.kind === 'scan'
        ? matPlan.finalRead.nodes[0].op.table
        : undefined;

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
