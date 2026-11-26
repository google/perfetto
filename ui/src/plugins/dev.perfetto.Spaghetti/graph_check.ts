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

import {getErrorMessage} from '../../base/errors';
import {shortUuid} from '../../base/uuid';
import type {Engine} from '../../trace_processor/engine';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import type {
  NodeData,
  NodeQueryBuilderStore,
  RootNodeData,
} from './graph_model';
import {
  chainTail,
  findDockedParent,
  flattenNodes,
  getManifestInputs,
  type GraphIndex,
  LabelSchema,
  resolveInputs,
  trimInputs,
  tryGetManifest,
} from './graph_utils';
import type {ChartConfig} from './nodes/chart';
import {serialize} from './serialize';
import type {SerializeResult} from './serialize_common';
import type {SqlDialect} from './sql_types';

export interface GraphNodeError {
  readonly nodeId: string;
  readonly title: string;
  readonly error: string;
}

export interface ValidatedSpaghettiGraph {
  readonly store: NodeQueryBuilderStore | undefined;
  readonly errors: readonly string[];
  readonly needsAutoLayout: boolean;
}

export interface SpaghettiGraphCheckResult {
  readonly errors: readonly GraphNodeError[];
  readonly targetNodeId: string | undefined;
  readonly sql: string | undefined;
}

function isPlainRecord(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function parseInputsArray(
  rawInputs: unknown,
  nodeLabel: string,
  errors: string[],
): (string | null)[] | undefined {
  if (rawInputs === undefined) return undefined;
  if (!Array.isArray(rawInputs)) {
    errors.push(`${nodeLabel}: "inputs" must be an array of string | null`);
    return undefined;
  }
  const result: (string | null)[] = [];
  for (const [idx, item] of rawInputs.entries()) {
    if (item === null || typeof item === 'string') {
      result.push(item === '' ? null : item);
    } else {
      errors.push(`${nodeLabel}: inputs[${idx}] must be a string or null`);
      result.push(null);
    }
  }
  const trimmed = trimInputs(result);
  return trimmed.length > 0 ? trimmed : undefined;
}

/**
 * Formats a successful `SerializeResult` with its `INCLUDE PERFETTO MODULE`
 * statements prepended, matching the SQL tab.
 */
export function formatSerializedSql(
  res: Extract<SerializeResult, {readonly ok: true}>,
): string {
  if (res.includes.length === 0) return res.sql;
  return [
    res.includes.map((inc) => `INCLUDE PERFETTO MODULE ${inc};`).join('\n'),
    res.sql,
  ].join('\n\n');
}

/**
 * Structurally validates and normalizes a candidate Spaghetti graph JSON string.
 * Aggregates all structural problems (bad JSON, unknown node types, Zod config
 * errors, duplicate IDs, docked source nodes, dangling inputs, port conflicts,
 * and cycles) before any UI state is mutated.
 */
export function validateSpaghettiGraphJson(
  json: string,
): ValidatedSpaghettiGraph {
  let raw: unknown;
  try {
    raw = JSON.parse(json);
  } catch (e) {
    return {
      store: undefined,
      errors: [`Invalid JSON: ${getErrorMessage(e)}`],
      needsAutoLayout: false,
    };
  }

  if (!isPlainRecord(raw)) {
    return {
      store: undefined,
      errors: ['Top-level JSON value must be an object with a "nodes" array'],
      needsAutoLayout: false,
    };
  }

  const errors: string[] = [];
  if (!Array.isArray(raw.nodes)) {
    errors.push('Top-level "nodes" must be an array');
    return {store: undefined, errors, needsAutoLayout: false};
  }

  const labels: NodeQueryBuilderStore['labels'] = [];
  if (raw.labels !== undefined) {
    if (!Array.isArray(raw.labels)) {
      errors.push('Top-level "labels" must be an array when provided');
    } else {
      for (const [idx, rawLabel] of raw.labels.entries()) {
        const parsed = LabelSchema.safeParse(rawLabel);
        if (parsed.success) {
          labels.push(parsed.data);
        } else {
          for (const issue of parsed.error.issues) {
            const path = issue.path.join('.');
            errors.push(
              `labels[${idx}]${path ? `.${path}` : ''}: ${issue.message}`,
            );
          }
        }
      }
    }
  }

  const seenIds = new Set<string>();
  let anyMissingCoords = false;

  function parseDockedNode(
    rawNode: unknown,
    pathLabel: string,
  ): NodeData | undefined {
    if (!isPlainRecord(rawNode)) {
      errors.push(`${pathLabel}: node must be an object`);
      return undefined;
    }
    const id = typeof rawNode.id === 'string' ? rawNode.id.trim() : '';
    const type = typeof rawNode.type === 'string' ? rawNode.type : '';
    const nodeLabel = id !== '' ? `node "${id}" (${type || '?'})` : pathLabel;

    if (id === '') {
      errors.push(`${pathLabel}: missing non-empty string "id"`);
    } else if (seenIds.has(id)) {
      errors.push(`${nodeLabel}: duplicate node id "${id}"`);
    } else {
      seenIds.add(id);
    }

    const manifest = type !== '' ? tryGetManifest(type) : undefined;
    if (!manifest) {
      errors.push(
        `${nodeLabel}: unknown node type "${type}" (check SPAGHETTI_GRAPH_FORMAT catalog)`,
      );
      return undefined;
    }

    const configRes = manifest.schema.safeParse(rawNode.config ?? {});
    if (!configRes.success) {
      for (const issue of configRes.error.issues) {
        const fieldPath = issue.path.join('.');
        errors.push(
          `${nodeLabel}: config${fieldPath ? `.${fieldPath}` : ''}: ${issue.message}`,
        );
      }
      return undefined;
    }

    const inputs = parseInputsArray(rawNode.inputs, nodeLabel, errors);
    const collapsed =
      typeof rawNode.collapsed === 'boolean' ? rawNode.collapsed : undefined;
    const next =
      rawNode.next !== undefined
        ? parseDockedNode(rawNode.next, `${nodeLabel}.next`)
        : undefined;

    const nodeData: NodeData = {
      id,
      type,
      config: configRes.data,
      ...(inputs !== undefined ? {inputs} : {}),
      ...(collapsed !== undefined ? {collapsed} : {}),
      ...(next !== undefined ? {next} : {}),
    };

    const ports = getManifestInputs(manifest, nodeData);
    if (ports.length === 0) {
      errors.push(
        `${nodeLabel}: source nodes have 0 input ports and cannot be docked inside "next" (place "${id}" in top-level "nodes" instead)`,
      );
    }

    return nodeData;
  }

  const roots: RootNodeData[] = [];
  for (const [rootIdx, rawRoot] of raw.nodes.entries()) {
    const pathLabel = `nodes[${rootIdx}]`;
    if (!isPlainRecord(rawRoot)) {
      errors.push(`${pathLabel}: root node must be an object`);
      continue;
    }

    const id = typeof rawRoot.id === 'string' ? rawRoot.id.trim() : '';
    const type = typeof rawRoot.type === 'string' ? rawRoot.type : '';
    const nodeLabel = id !== '' ? `node "${id}" (${type || '?'})` : pathLabel;

    if (id === '') {
      errors.push(`${pathLabel}: missing non-empty string "id"`);
    } else if (seenIds.has(id)) {
      errors.push(`${nodeLabel}: duplicate node id "${id}"`);
    } else {
      seenIds.add(id);
    }

    let x = 80 + rootIdx * 320;
    let y = 80;
    if (rawRoot.x === undefined || rawRoot.y === undefined) {
      anyMissingCoords = true;
    }
    if (rawRoot.x !== undefined) {
      if (typeof rawRoot.x === 'number' && Number.isFinite(rawRoot.x)) {
        x = rawRoot.x;
      } else {
        errors.push(`${nodeLabel}: "x" must be a finite number`);
      }
    }
    if (rawRoot.y !== undefined) {
      if (typeof rawRoot.y === 'number' && Number.isFinite(rawRoot.y)) {
        y = rawRoot.y;
      } else {
        errors.push(`${nodeLabel}: "y" must be a finite number`);
      }
    }

    const manifest = type !== '' ? tryGetManifest(type) : undefined;
    if (!manifest) {
      errors.push(
        `${nodeLabel}: unknown node type "${type}" (check SPAGHETTI_GRAPH_FORMAT catalog)`,
      );
      continue;
    }

    const configRes = manifest.schema.safeParse(rawRoot.config ?? {});
    if (!configRes.success) {
      for (const issue of configRes.error.issues) {
        const fieldPath = issue.path.join('.');
        errors.push(
          `${nodeLabel}: config${fieldPath ? `.${fieldPath}` : ''}: ${issue.message}`,
        );
      }
      continue;
    }

    const inputs = parseInputsArray(rawRoot.inputs, nodeLabel, errors);
    const collapsed =
      typeof rawRoot.collapsed === 'boolean' ? rawRoot.collapsed : undefined;
    const next =
      rawRoot.next !== undefined
        ? parseDockedNode(rawRoot.next, `${nodeLabel}.next`)
        : undefined;

    if (id !== '') {
      roots.push({
        id,
        type,
        x,
        y,
        config: configRes.data,
        ...(inputs !== undefined ? {inputs} : {}),
        ...(collapsed !== undefined ? {collapsed} : {}),
        ...(next !== undefined ? {next} : {}),
      });
    }
  }

  if (errors.length > 0) {
    return {store: undefined, errors, needsAutoLayout: false};
  }

  const index = flattenNodes(roots);

  // Validate wiring & normalize redundant docked port-0 references.
  for (const node of Object.values(index.nodes)) {
    const manifest = tryGetManifest(node.type);
    if (!manifest) continue;
    const ports = getManifestInputs(manifest, node);
    const dockedParent = findDockedParent(index, node.id);

    if (node.inputs) {
      let normalized = false;
      const updatedInputs = [...node.inputs];

      for (const [portIdx, refId] of updatedInputs.entries()) {
        if (refId === null) continue;
        if (refId === node.id) {
          errors.push(
            `node "${node.id}" (${node.type}): self-loop in inputs[${portIdx}]`,
          );
          continue;
        }
        if (index.nodes[refId] === undefined) {
          errors.push(
            `node "${node.id}" (${node.type}): inputs[${portIdx}] references unknown node "${refId}"`,
          );
          continue;
        }
        if (portIdx >= ports.length) {
          errors.push(
            `node "${node.id}" (${node.type}): inputs[${portIdx}] exceeds node's ${ports.length} input port(s)`,
          );
          continue;
        }
        if (portIdx === 0 && dockedParent !== undefined) {
          if (refId === dockedParent.id) {
            updatedInputs[0] = null;
            normalized = true;
          } else {
            errors.push(
              `node "${node.id}" (${node.type}): port 0 is already connected via docking from "${dockedParent.id}"; cannot also wire inputs[0] to "${refId}"`,
            );
          }
        }
      }

      if (normalized) {
        const trimmed = trimInputs(updatedInputs);
        node.inputs = trimmed.length > 0 ? trimmed : undefined;
      }
    }
  }

  // Detect cycles across docking (`next`) + wired (`inputs`) edges.
  const color = new Map<string, 'visiting' | 'visited'>();
  const stack: string[] = [];

  function dfs(nodeId: string): void {
    const state = color.get(nodeId);
    if (state === 'visited') return;
    if (state === 'visiting') {
      const cycleStart = stack.indexOf(nodeId);
      const cyclePath = [...stack.slice(cycleStart), nodeId].join(' -> ');
      errors.push(`Cycle detected in graph: ${cyclePath}`);
      return;
    }

    color.set(nodeId, 'visiting');
    stack.push(nodeId);

    const node = index.nodes[nodeId];
    if (node) {
      const dockedParent = findDockedParent(index, nodeId);
      if (dockedParent) {
        dfs(dockedParent.id);
      }
      for (const refId of node.inputs ?? []) {
        if (refId !== null && index.nodes[refId] !== undefined) {
          dfs(refId);
        }
      }
    }

    stack.pop();
    color.set(nodeId, 'visited');
  }

  for (const id of Object.keys(index.nodes)) {
    if (!color.has(id)) {
      dfs(id);
    }
  }

  if (errors.length > 0) {
    return {store: undefined, errors, needsAutoLayout: false};
  }

  return {
    store: {nodes: roots, labels},
    errors: [],
    needsAutoLayout: anyMissingCoords,
  };
}

function computeTopologicalOrder(
  index: GraphIndex,
  rootIds?: readonly string[],
): string[] {
  const order: string[] = [];
  const visited = new Set<string>();
  const visiting = new Set<string>();

  function visit(nodeId: string): void {
    if (visited.has(nodeId) || visiting.has(nodeId)) return;
    const node = index.nodes[nodeId];
    if (!node) return;
    visiting.add(nodeId);
    for (const inp of resolveInputs(index, nodeId)) {
      visit(inp.upstreamId);
    }
    visiting.delete(nodeId);
    visited.add(nodeId);
    order.push(nodeId);
  }

  for (const id of rootIds ?? Object.keys(index.nodes)) {
    visit(id);
  }
  return order;
}

async function testExecuteSerialized(
  engine: Engine,
  serialized: Extract<SerializeResult, {readonly ok: true}>,
  dialect: SqlDialect,
  includedModules: Set<string>,
  tempTables: string[],
): Promise<{readonly error?: string; readonly subquerySql?: string}> {
  for (const inc of serialized.includes) {
    if (!includedModules.has(inc)) {
      try {
        await engine.query(`INCLUDE PERFETTO MODULE ${inc}`);
        includedModules.add(inc);
      } catch (e) {
        return {error: getErrorMessage(e)};
      }
    }
  }

  const cleanSql = serialized.sql.trim().replace(/;+$/, '');
  if (dialect === 'sql') {
    try {
      await engine.query(`SELECT * FROM (\n${cleanSql}\n) LIMIT 1`);
      return {subquerySql: cleanSql};
    } catch (e) {
      return {error: getErrorMessage(e)};
    }
  }

  // PerfettoSQL Next ('pipe') queries cannot be wrapped directly in
  // `SELECT * FROM (...)`; materialize into an isolated temp table and drop it.
  const tempName = `_qb_chk_${shortUuid().replace(/[^a-zA-Z0-9_]/g, '')}`;
  tempTables.push(tempName);
  try {
    await engine.query(`CREATE PERFETTO TABLE ${tempName} AS\n${cleanSql}`);
    await engine.query(`SELECT * FROM ${tempName} LIMIT 1`);
    return {subquerySql: `SELECT * FROM ${tempName}`};
  } catch (e) {
    return {error: getErrorMessage(e)};
  }
}

/**
 * Serializes and test-executes nodes in the Spaghetti graph against `engine`,
 * returning any per-node serialization or SQL runtime errors alongside the
 * generated SQL for the target node.
 */
export async function checkSpaghettiGraph(
  engine: Engine,
  store: NodeQueryBuilderStore,
  sqlModules: SqlModules | undefined,
  dialect: SqlDialect = 'sql',
  targetNodeId?: string,
): Promise<SpaghettiGraphCheckResult> {
  const index = flattenNodes(store.nodes);
  const resolvedTargetId = targetNodeId ?? pickDefaultTargetNodeId(store);

  if (targetNodeId !== undefined && index.nodes[targetNodeId] === undefined) {
    return {
      errors: [
        {
          nodeId: targetNodeId,
          title: targetNodeId,
          error: `Node "${targetNodeId}" does not exist in the current graph`,
        },
      ],
      targetNodeId,
      sql: undefined,
    };
  }

  const order = computeTopologicalOrder(
    index,
    targetNodeId !== undefined ? [targetNodeId] : undefined,
  );

  const errors: GraphNodeError[] = [];
  const failedNodeIds = new Set<string>();
  const includedModules = new Set<string>();
  const tempTables: string[] = [];
  let targetSql: string | undefined;

  try {
    for (const nodeId of order) {
      const node = index.nodes[nodeId];
      if (!node) continue;
      const manifest = tryGetManifest(node.type);
      const title = manifest?.title ?? node.type;

      // Skip nodes whose upstream already failed so we report root causes once.
      const inputs = resolveInputs(index, nodeId);
      if (inputs.some((inp) => failedNodeIds.has(inp.upstreamId))) {
        failedNodeIds.add(nodeId);
        continue;
      }

      const serialized = serialize(index, nodeId, dialect, {sqlModules});
      if (!serialized.ok) {
        errors.push({
          nodeId: serialized.nodeId ?? nodeId,
          title,
          error: serialized.error,
        });
        failedNodeIds.add(nodeId);
        continue;
      }

      if (nodeId === resolvedTargetId) {
        targetSql = formatSerializedSql(serialized);
      }

      const runtimeRes = await testExecuteSerialized(
        engine,
        serialized,
        dialect,
        includedModules,
        tempTables,
      );
      if (runtimeRes.error !== undefined) {
        errors.push({
          nodeId,
          title,
          error: runtimeRes.error,
        });
        failedNodeIds.add(nodeId);
        continue;
      }

      // Validate Chart specs against the query output so invalid xCol/yCol
      // names are caught before rendering.
      if (node.type === 'chart' && runtimeRes.subquerySql !== undefined) {
        const chartConfig = node.config as ChartConfig;
        for (const spec of chartConfig.charts) {
          if (spec.xCol === '') continue;
          const valExpr = spec.yCol
            ? `SUM(CAST(${spec.yCol} AS REAL))`
            : 'COUNT(*)';
          const chartCheckSql = [
            `SELECT CAST(${spec.xCol} AS TEXT) AS label, ${valExpr} AS val`,
            `FROM (\n${runtimeRes.subquerySql}\n)`,
            `WHERE ${spec.xCol} IS NOT NULL`,
            'GROUP BY 1',
            'LIMIT 1',
          ].join('\n');
          try {
            await engine.query(chartCheckSql);
          } catch (e) {
            errors.push({
              nodeId,
              title,
              error: `Chart (${spec.xCol}${spec.yCol ? ` × ${spec.yCol}` : ''}): ${getErrorMessage(e)}`,
            });
            failedNodeIds.add(nodeId);
            break;
          }
        }
      }
    }
  } finally {
    for (const table of tempTables) {
      await engine.tryQuery(`DROP TABLE IF EXISTS ${table}`).catch(() => {});
    }
  }

  return {
    errors,
    targetNodeId: resolvedTargetId,
    sql: targetSql,
  };
}

export function formatSpaghettiGraphErrors(
  errors: ReadonlyArray<GraphNodeError>,
): string {
  return errors
    .map((e) => `- node ${e.nodeId} (${e.title}): ${e.error}`)
    .join('\n');
}

/**
 * Picks a sensible default sink node to select when a new graph is applied:
 * prefers the last root chain's tail that has no downstream wired consumers.
 */
export function pickDefaultTargetNodeId(
  store: NodeQueryBuilderStore,
): string | undefined {
  if (store.nodes.length === 0) return undefined;
  const index = flattenNodes(store.nodes);
  const consumedIds = new Set<string>();
  for (const node of Object.values(index.nodes)) {
    for (const refId of node.inputs ?? []) {
      if (refId !== null) consumedIds.add(refId);
    }
  }
  const tails = store.nodes.map((root) => chainTail(root).id);
  for (let i = tails.length - 1; i >= 0; i--) {
    const tailId = tails[i];
    if (tailId !== undefined && !consumedIds.has(tailId)) {
      return tailId;
    }
  }
  return tails[tails.length - 1];
}
