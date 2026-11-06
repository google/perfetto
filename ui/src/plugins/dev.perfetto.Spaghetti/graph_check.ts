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
  findConnectedInputs,
  findDockedParent,
  flattenNodes,
  getManifestInputs,
  type GraphIndex,
  isNodeValid,
  LabelSchema,
  trimInputs,
  tryGetManifest,
} from './graph_utils';
import {
  buildLogicalPlan,
  formatLogicalPlan,
  formatSql,
  type LogicalPlan,
  type SqlDialect,
} from './ir';
import {planMaterialization} from './materialization';
import type {ChartConfig} from './nodes/chart';

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

  // Auto-layout if any root omitted coordinates or multiple roots are stacked
  // at the exact same point (e.g. all {x: 0, y: 0}).
  const firstRoot = roots[0];
  const allStackedAtSamePos =
    roots.length > 1 &&
    firstRoot !== undefined &&
    roots.every((r) => r.x === firstRoot.x && r.y === firstRoot.y);
  if (allStackedAtSamePos) {
    for (const [idx, root] of roots.entries()) {
      root.x = 80 + idx * 320;
      root.y = 80;
    }
  }

  const needsAutoLayout =
    roots.length > 0 && (anyMissingCoords || allStackedAtSamePos);

  return {
    store: {nodes: roots, labels},
    errors: [],
    needsAutoLayout,
  };
}

export function describeInvalidNodeConfig(node: NodeData): string {
  switch (node.type) {
    case 'from':
      return 'Table name must not be empty';
    case 'time_range':
      return 'Time range requires non-zero ts or dur';
    case 'data_table':
      return 'Data Table has empty/invalid column names or cell values matching column types';
    case 'as':
      return 'Alias must be a valid SQL identifier';
    case 'filter':
      return 'Filter condition is missing a comparison value';
    case 'sort':
      return 'Sort requires at least one non-empty sort column';
    case 'limit':
      return 'Limit requires non-negative integer digits in limitCount (and optional offset)';
    case 'groupby':
      return 'Group By requires at least one non-empty column in groupColumns';
    case 'join':
      return 'Join requires both leftColumn and rightColumn to be specified';
    case 'intervals_from_events':
      return 'Intervals From Events requires a non-empty closeAtExpr when lastMode is CLOSE_AT';
    case 'tree_accumulate':
      return 'Tree Accumulate requires non-empty idColumn and parentIdColumn';
    case 'graph_tree':
      return 'Graph Tree requires non-empty nodeIdColumn, sourceColumn, destColumn, and rootIdColumn';
    case 'sql':
      return 'SQL query must not be empty';
    default:
      return 'Invalid or incomplete node configuration';
  }
}

function computeTopologicalOrder(index: GraphIndex): {
  readonly order: readonly NodeData[];
  readonly cycleNodeIds: ReadonlySet<string>;
} {
  const state = new Map<string, 'visiting' | 'visited'>();
  const order: NodeData[] = [];
  const cycleNodeIds = new Set<string>();

  function visit(nodeId: string): void {
    const cur = state.get(nodeId);
    if (cur === 'visited') return;
    if (cur === 'visiting') {
      cycleNodeIds.add(nodeId);
      return;
    }
    const node = index.nodes[nodeId];
    if (!node) return;

    state.set(nodeId, 'visiting');
    const manifest = tryGetManifest(node.type);
    if (manifest) {
      const ports = getManifestInputs(manifest, node);
      const dockedParent = findDockedParent(index, nodeId);
      const connected = findConnectedInputs(index, nodeId);
      for (let i = 0; i < ports.length; i++) {
        const inp = i === 0 && dockedParent ? dockedParent : connected.get(i);
        if (inp) {
          visit(inp.id);
        }
      }
    }
    state.set(nodeId, 'visited');
    order.push(node);
  }

  for (const id of Object.keys(index.nodes)) {
    visit(id);
  }

  return {order, cycleNodeIds};
}

async function checkLogicalPlanRuntime(
  engine: Engine,
  plan: LogicalPlan,
  dialect: SqlDialect,
  includedModules: Set<string>,
  tempTables: string[],
): Promise<{readonly error?: string; readonly subquerySql?: string}> {
  if (dialect === 'sql') {
    const formatted = formatSql(plan, {includeModuleHeaders: false});
    if (!formatted.ok) {
      return {error: formatted.error};
    }
    for (const inc of formatted.includes) {
      if (!includedModules.has(inc)) {
        try {
          await engine.query(`INCLUDE PERFETTO MODULE ${inc}`);
          includedModules.add(inc);
        } catch (e) {
          return {error: getErrorMessage(e)};
        }
      }
    }
    const cleanSql = formatted.sql.trim().replace(/;+$/, '');
    try {
      await engine.query(`SELECT * FROM (\n${cleanSql}\n) LIMIT 1`);
      return {subquerySql: cleanSql};
    } catch (e) {
      return {error: getErrorMessage(e)};
    }
  }

  // PerfettoSQL Next ('pipe') dialect check via isolated temporary tables.
  const matPlan = planMaterialization(plan);
  for (const inc of matPlan.includes) {
    if (!includedModules.has(inc)) {
      try {
        await engine.query(`INCLUDE PERFETTO MODULE ${inc}`);
        includedModules.add(inc);
      } catch (e) {
        return {error: getErrorMessage(e)};
      }
    }
  }

  const runPrefix = `_qb_chk_${shortUuid().replace(/[^a-zA-Z0-9_]/g, '')}_`;
  const hashRemap = new Map<string, string>();
  for (const [idx, step] of matPlan.steps.entries()) {
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

  for (const step of matPlan.steps) {
    const res = formatLogicalPlan(step.plan, 'pipe', {
      includeModuleHeaders: false,
    });
    if (!res.ok) return {error: res.error};
    const tempName = hashRemap.get(step.hash);
    if (tempName === undefined) continue;
    try {
      await engine.query(
        `CREATE OR REPLACE PERFETTO TABLE ${tempName} AS\n${remapSql(res.sql)}`,
      );
    } catch (e) {
      return {error: getErrorMessage(e)};
    }
  }

  const finalRes = formatSql(matPlan.finalRead, {includeModuleHeaders: false});
  if (!finalRes.ok) return {error: finalRes.error};
  const finalSql = remapSql(finalRes.sql).trim().replace(/;+$/, '');
  try {
    await engine.query(`SELECT * FROM (\n${finalSql}\n) LIMIT 1`);
    return {subquerySql: finalSql};
  } catch (e) {
    return {error: getErrorMessage(e)};
  }
}

/**
 * Checks every node in `store` in topological order for invalid configs,
 * unconnected required ports, and TraceProcessor SQL execution errors,
 * attributing failures to the earliest failing node without leaving any
 * side-effect tables behind.
 */
export async function collectSpaghettiGraphErrors(
  engine: Engine,
  store: NodeQueryBuilderStore,
  sqlModules: SqlModules | undefined,
  dialect: SqlDialect = 'sql',
): Promise<GraphNodeError[]> {
  if (store.nodes.length === 0) return [];

  const index = flattenNodes(store.nodes);
  const {order, cycleNodeIds} = computeTopologicalOrder(index);
  const errors: GraphNodeError[] = [];
  const failedNodeIds = new Set<string>();
  const includedModules = new Set<string>();
  const tempTables: string[] = [];

  for (const cycleId of cycleNodeIds) {
    const node = index.nodes[cycleId];
    const title = (node && tryGetManifest(node.type)?.title) ?? cycleId;
    errors.push({
      nodeId: cycleId,
      title,
      error: 'Node is part of a dependency cycle',
    });
    failedNodeIds.add(cycleId);
  }

  try {
    for (const node of order) {
      if (failedNodeIds.has(node.id)) continue;

      const manifest = tryGetManifest(node.type);
      if (!manifest) {
        errors.push({
          nodeId: node.id,
          title: node.type,
          error: `Unknown node type "${node.type}"`,
        });
        failedNodeIds.add(node.id);
        continue;
      }

      const ports = getManifestInputs(manifest, node);
      const dockedParent = findDockedParent(index, node.id);
      const connected = findConnectedInputs(index, node.id);

      // Skip downstream nodes when an upstream dependency already failed so we
      // only report root-cause errors.
      let upstreamFailed = false;
      for (let i = 0; i < ports.length; i++) {
        const inp = i === 0 && dockedParent ? dockedParent : connected.get(i);
        if (inp && failedNodeIds.has(inp.id)) {
          upstreamFailed = true;
          break;
        }
      }
      if (upstreamFailed) {
        failedNodeIds.add(node.id);
        continue;
      }

      if (!isNodeValid(node)) {
        errors.push({
          nodeId: node.id,
          title: manifest.title,
          error: describeInvalidNodeConfig(node),
        });
        failedNodeIds.add(node.id);
        continue;
      }

      let missingPortError: string | undefined;
      for (const [portIdx, port] of ports.entries()) {
        if (port.optional) continue;
        const inp =
          portIdx === 0 && dockedParent ? dockedParent : connected.get(portIdx);
        if (!inp) {
          missingPortError = `Missing input connection on port ${portIdx} ("${port.name}")`;
          break;
        }
      }
      if (missingPortError !== undefined) {
        errors.push({
          nodeId: node.id,
          title: manifest.title,
          error: missingPortError,
        });
        failedNodeIds.add(node.id);
        continue;
      }

      const logicalPlan = buildLogicalPlan(index, node.id, sqlModules);
      if (!logicalPlan) {
        errors.push({
          nodeId: node.id,
          title: manifest.title,
          error: 'Could not build query plan (incomplete upstream inputs)',
        });
        failedNodeIds.add(node.id);
        continue;
      }

      const runtimeRes = await checkLogicalPlanRuntime(
        engine,
        logicalPlan,
        dialect,
        includedModules,
        tempTables,
      );
      if (runtimeRes.error !== undefined) {
        errors.push({
          nodeId: node.id,
          title: manifest.title,
          error: runtimeRes.error,
        });
        failedNodeIds.add(node.id);
        continue;
      }

      // Also validate Chart specs against the upstream query output so invalid
      // xCol/yCol names are caught before rendering.
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
              nodeId: node.id,
              title: manifest.title,
              error: `Chart (${spec.xCol}${spec.yCol ? ` × ${spec.yCol}` : ''}): ${getErrorMessage(e)}`,
            });
            failedNodeIds.add(node.id);
            break;
          }
        }
      }
    }
  } finally {
    for (const table of tempTables) {
      await engine.query(`DROP TABLE IF EXISTS ${table}`).catch(() => {});
    }
  }

  return errors;
}

export function formatSpaghettiGraphErrors(
  errors: ReadonlyArray<GraphNodeError>,
): string {
  return errors
    .map((e) => `- node ${e.nodeId} (${e.title}): ${e.error}`)
    .join('\n');
}

/**
 * Walks the upstream dependency DAG of `targetNodeId` to identify the root-cause
 * incomplete config, unconnected required port, or dependency cycle preventing
 * a `LogicalPlan` from being constructed.
 */
export function findUpstreamPlanBlocker(
  index: GraphIndex,
  targetNodeId: string,
): GraphNodeError | undefined {
  const state = new Map<string, 'visiting' | 'visited'>();

  function visit(nodeId: string): GraphNodeError | undefined {
    const cur = state.get(nodeId);
    if (cur === 'visited') return undefined;
    const node = index.nodes[nodeId];
    if (!node) {
      return {
        nodeId,
        title: nodeId,
        error: `Node "${nodeId}" does not exist`,
      };
    }
    const manifest = tryGetManifest(node.type);
    const title = manifest?.title ?? node.type;
    if (cur === 'visiting') {
      return {
        nodeId,
        title,
        error: 'Node is part of a dependency cycle',
      };
    }
    if (!manifest) {
      return {
        nodeId,
        title,
        error: `Unknown node type "${node.type}"`,
      };
    }

    state.set(nodeId, 'visiting');
    const ports = getManifestInputs(manifest, node);
    const dockedParent = findDockedParent(index, nodeId);
    const connected = findConnectedInputs(index, nodeId);

    for (const [portIdx, port] of ports.entries()) {
      const inp =
        portIdx === 0 && dockedParent ? dockedParent : connected.get(portIdx);
      if (!inp) {
        if (!port.optional) {
          return {
            nodeId,
            title,
            error: `Missing input connection on port ${portIdx} ("${port.name}")`,
          };
        }
        continue;
      }
      const upstreamErr = visit(inp.id);
      if (upstreamErr !== undefined) return upstreamErr;
    }

    if (!isNodeValid(node)) {
      return {
        nodeId,
        title,
        error: describeInvalidNodeConfig(node),
      };
    }

    state.set(nodeId, 'visited');
    return undefined;
  }

  return visit(targetNodeId);
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
