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

import {shortUuid} from '../../base/uuid';
import type {Engine} from '../../trace_processor/engine';
import {
  NUM_NULL,
  STR_NULL,
  type SqlValue,
} from '../../trace_processor/query_result';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import {findUpstreamPlanBlocker, pickDefaultTargetNodeId} from './graph_check';
import type {NodeQueryBuilderStore} from './graph_model';
import {
  flattenNodes,
  getOutputColumnsForNode,
  tryGetManifest,
} from './graph_utils';
import {
  buildDisplayQuery,
  buildLogicalPlan,
  formatLogicalPlan,
  formatSql,
  type LogicalPlan,
  type SqlDialect,
} from './ir';
import {
  computeNeededStepHashes,
  type MaterializationService,
  planMaterialization,
} from './materialization';
import type {ChartConfig} from './nodes/chart';

export const DEFAULT_EVAL_ROW_LIMIT = 50;
export const MAX_EVAL_ROW_LIMIT = 200;
const CHART_BAR_LIMIT = 20;

export type JsonSqlValue = string | number | null;

export interface ChartBarEvaluation {
  readonly label: string;
  readonly value: number;
}

export interface ChartSeriesEvaluation {
  readonly xCol: string;
  readonly yCol: string;
  readonly bars: readonly ChartBarEvaluation[];
}

export interface SpaghettiNodeEvaluation {
  readonly nodeId: string;
  readonly type: string;
  readonly title: string;
  readonly sql: string;
  readonly totalRowCount: number;
  readonly columns: readonly string[];
  readonly rows: ReadonlyArray<Record<string, JsonSqlValue>>;
  readonly offset: number;
  readonly limit: number;
  readonly truncated: boolean;
  readonly charts?: readonly ChartSeriesEvaluation[];
}

export interface EvaluateSpaghettiNodeOptions {
  readonly engine: Engine;
  readonly store: NodeQueryBuilderStore;
  readonly sqlModules: SqlModules | undefined;
  readonly dialect: SqlDialect;
  readonly nodeId?: string;
  readonly activeNodeId?: string;
  readonly matService?: MaterializationService;
  readonly limit?: number;
  readonly offset?: number;
}

export function serializeSqlValue(value: SqlValue): JsonSqlValue {
  if (typeof value === 'bigint') {
    const asNumber = Number(value);
    return Number.isSafeInteger(asNumber) ? asNumber : String(value);
  }
  if (value instanceof Uint8Array) {
    return `<blob ${value.byteLength}B>`;
  }
  return value;
}

interface PreparedNodeReadQuery {
  readonly readSql: string;
  readonly tempTables: readonly string[];
}

async function prepareNodeReadQuery(
  engine: Engine,
  logicalPlan: LogicalPlan,
  targetId: string,
  dialect: SqlDialect,
  matService: MaterializationService | undefined,
): Promise<PreparedNodeReadQuery> {
  // Fast path: reuse the active node's already-materialized read query directly.
  if (
    matService !== undefined &&
    matService.evaluatedNodeId === targetId &&
    matService.activeDialect === dialect &&
    matService.readQuerySql !== undefined &&
    matService.error === undefined
  ) {
    return {readSql: matService.readQuerySql, tempTables: []};
  }

  const warmHashes = matService?.warmHashes ?? new Set<string>();
  const matPlan = planMaterialization(logicalPlan, {warmHashes});
  const neededHashes = computeNeededStepHashes(matPlan, warmHashes);

  let usesWarmHash = false;
  for (const hash of neededHashes) {
    if (warmHashes.has(hash)) {
      usesWarmHash = true;
      break;
    }
  }

  // In pure SQL dialect with no warm upstream tables to reuse, execute as a
  // single CTE query without creating/dropping intermediate tables.
  if (dialect === 'sql' && !usesWarmHash) {
    const formatted = formatSql(logicalPlan, {includeModuleHeaders: false});
    if (!formatted.ok) {
      throw new Error(formatted.error);
    }
    for (const inc of formatted.includes) {
      await engine.query(`INCLUDE PERFETTO MODULE ${inc}`);
    }
    return {
      readSql: formatted.sql.trim().replace(/;+$/, ''),
      tempTables: [],
    };
  }

  for (const inc of matPlan.includes) {
    await engine.query(`INCLUDE PERFETTO MODULE ${inc}`);
  }

  const runPrefix = `_qb_eval_${shortUuid().replace(/[^a-zA-Z0-9_]/g, '')}_`;
  const hashRemap = new Map<string, string>();
  const tempTables: string[] = [];

  for (const [idx, step] of matPlan.steps.entries()) {
    if (!neededHashes.has(step.hash) || warmHashes.has(step.hash)) continue;
    const tempName = `${runPrefix}${idx}`;
    hashRemap.set(step.hash, tempName);
    tempTables.push(tempName);
  }

  function remapSql(sql: string): string {
    let out = sql;
    for (const [origHash, tempName] of hashRemap) {
      out = out.replaceAll(origHash, tempName);
    }
    return out;
  }

  try {
    for (const step of matPlan.steps) {
      const tempName = hashRemap.get(step.hash);
      if (tempName === undefined) continue;
      const stepRes = formatLogicalPlan(step.plan, dialect, {
        includeModuleHeaders: false,
      });
      if (!stepRes.ok) {
        throw new Error(stepRes.error);
      }
      await engine.query(
        `CREATE OR REPLACE PERFETTO TABLE ${tempName} AS\n${remapSql(stepRes.sql)}`,
      );
    }

    const singleScan =
      matPlan.finalRead.nodes.length === 1 &&
      matPlan.finalRead.nodes[0]?.op.kind === 'scan'
        ? matPlan.finalRead.nodes[0].op.table
        : undefined;

    if (singleScan !== undefined) {
      return {
        readSql: `SELECT * FROM ${remapSql(singleScan)}`,
        tempTables,
      };
    }

    const finalRes = formatSql(matPlan.finalRead, {
      includeModuleHeaders: false,
    });
    if (!finalRes.ok) {
      throw new Error(finalRes.error);
    }
    return {
      readSql: remapSql(finalRes.sql).trim().replace(/;+$/, ''),
      tempTables,
    };
  } catch (e) {
    for (const table of tempTables) {
      await engine.query(`DROP TABLE IF EXISTS ${table}`).catch(() => {});
    }
    throw e;
  }
}

/**
 * Evaluates a specific node (or the active/default sink node) in `store`
 * against `engine` without mutating UI selection, reusing warm cached `_qb_*`
 * tables from `matService` when available.
 */
export async function evaluateSpaghettiNode(
  options: EvaluateSpaghettiNodeOptions,
): Promise<SpaghettiNodeEvaluation> {
  const {engine, store, sqlModules, dialect, matService} = options;
  if (store.nodes.length === 0) {
    throw new Error('Spaghetti graph is empty.');
  }

  const index = flattenNodes(store.nodes);
  const targetId =
    options.nodeId ??
    options.activeNodeId ??
    matService?.evaluatedNodeId ??
    pickDefaultTargetNodeId(store);

  if (targetId === undefined) {
    throw new Error('Spaghetti graph has no evaluatable node.');
  }

  const node = index.nodes[targetId];
  if (node === undefined) {
    const knownIds = Object.keys(index.nodes);
    throw new Error(
      `Node "${targetId}" was not found in the Spaghetti graph (available node ids: ${knownIds.join(', ')}).`,
    );
  }

  const manifest = tryGetManifest(node.type);
  const title = manifest?.title ?? node.type;
  const logicalPlan = buildLogicalPlan(index, targetId, sqlModules);
  if (!logicalPlan) {
    const blocker = findUpstreamPlanBlocker(index, targetId);
    if (blocker !== undefined) {
      if (blocker.nodeId === targetId) {
        throw new Error(
          `Cannot evaluate node "${targetId}" (${title}): ${blocker.error}`,
        );
      }
      throw new Error(
        `Cannot evaluate node "${targetId}" (${title}): upstream node "${blocker.nodeId}" (${blocker.title}) failed: ${blocker.error}`,
      );
    }
    throw new Error(
      `Cannot evaluate node "${targetId}" (${title}): incomplete node configuration or upstream inputs.`,
    );
  }

  const displaySql = buildDisplayQuery(logicalPlan, dialect) ?? '';
  const limit = Math.max(
    0,
    Math.min(
      MAX_EVAL_ROW_LIMIT,
      Math.trunc(options.limit ?? DEFAULT_EVAL_ROW_LIMIT),
    ),
  );
  const offset = Math.max(0, Math.trunc(options.offset ?? 0));

  const {readSql, tempTables} = await prepareNodeReadQuery(
    engine,
    logicalPlan,
    targetId,
    dialect,
    matService,
  );

  try {
    const countRes = await engine.query(
      `SELECT COUNT(*) AS cnt FROM (\n${readSql}\n)`,
    );
    let totalRowCount = 0;
    const countIt = countRes.iter({cnt: NUM_NULL});
    if (countIt.valid()) {
      totalRowCount = Number(countIt.cnt ?? 0);
    }

    const rowRes = await engine.query(
      `SELECT * FROM (\n${readSql}\n) LIMIT ${limit} OFFSET ${offset}`,
    );
    const resultCols = rowRes.columns();
    const columns =
      resultCols.length > 0
        ? resultCols
        : (getOutputColumnsForNode(index, targetId, sqlModules) ?? []).map(
            (c) => c.name,
          );

    const rows: Array<Record<string, JsonSqlValue>> = [];
    for (const it = rowRes.iter({}); it.valid(); it.next()) {
      const row: Record<string, JsonSqlValue> = {};
      for (const colName of columns) {
        row[colName] = serializeSqlValue(it.get(colName));
      }
      rows.push(row);
    }

    let charts: ChartSeriesEvaluation[] | undefined;
    if (node.type === 'chart') {
      const chartConfig = node.config as ChartConfig;
      charts = [];
      for (const spec of chartConfig.charts) {
        if (spec.xCol === '') continue;
        const valueExpr = spec.yCol
          ? `SUM(CAST(${spec.yCol} AS REAL))`
          : 'COUNT(*)';
        const chartSql = [
          `SELECT CAST(${spec.xCol} AS TEXT) AS label, ${valueExpr} AS value`,
          `FROM (\n${readSql}\n)`,
          `GROUP BY ${spec.xCol}`,
          `ORDER BY value DESC`,
          `LIMIT ${CHART_BAR_LIMIT}`,
        ].join('\n');
        const chartRes = await engine.query(chartSql);
        const bars: ChartBarEvaluation[] = [];
        for (
          const it = chartRes.iter({label: STR_NULL, value: NUM_NULL});
          it.valid();
          it.next()
        ) {
          bars.push({
            label: it.label ?? '(null)',
            value: Number(it.value ?? 0),
          });
        }
        charts.push({
          xCol: spec.xCol,
          yCol: spec.yCol,
          bars,
        });
      }
    }

    return {
      nodeId: node.id,
      type: node.type,
      title,
      sql: displaySql,
      totalRowCount,
      columns,
      rows,
      offset,
      limit,
      truncated: offset + rows.length < totalRowCount,
      ...(charts !== undefined ? {charts} : {}),
    };
  } finally {
    for (const table of tempTables) {
      await engine.query(`DROP TABLE IF EXISTS ${table}`).catch(() => {});
    }
  }
}
