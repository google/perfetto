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

import {Connection} from '../../widgets/nodegraph';
import {PerfettoSqlType} from '../../trace_processor/perfetto_sql_type';
import {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import {FilterCondition} from './filter';
import {IntervalIntersectNodeData} from './interval_intersect';
import {getExtendColumnAliases} from './extend';
import {NodeData} from './node_types';

// A column definition with optional type information.
export interface ColumnDef {
  readonly name: string;
  readonly type?: PerfettoSqlType;
}

const UNARY_OPS: Set<string> = new Set(['IS NULL', 'IS NOT NULL']);

// FNV-1a hash → 8-char hex string. Used for IR entry hashing.
export function fnvHash(s: string): string {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 16777619);
  }
  return (h >>> 0).toString(16).padStart(8, '0');
}

// Check if a single node's configuration is valid for SQL generation.
export function isNodeValid(
  node: NodeData,
  nodes: Map<string, NodeData>,
  connections: Connection[],
): boolean {
  switch (node.type) {
    case 'from':
      return node.table !== '';
    case 'selection':
      return node.ts !== '0' || node.dur !== '0';
    case 'select':
      return node.expressions.every(
        (e) => (!e.expression && !e.alias) || (e.expression && e.alias),
      );
    case 'extract_arg':
      return node.extractions.every(
        (e) =>
          (!e.column && !e.argName && !e.alias) ||
          (e.column && e.argName && e.alias),
      );
    case 'filter':
      return isFilterValid(node.conditions);
    case 'extend': {
      const inputs = findConnectedInputs(nodes, connections, node.id);
      const hasDockedParent = findDockedParent(nodes, node.id) !== undefined;
      const hasLeft = hasDockedParent || inputs.has(0);
      const hasRight = inputs.has(1);
      return (
        hasLeft && hasRight && node.leftColumn !== '' && node.rightColumn !== ''
      );
    }
    case 'interval_intersect':
    case 'union_all': {
      const inputs = findConnectedInputs(nodes, connections, node.id);
      const hasDockedParent = findDockedParent(nodes, node.id) !== undefined;
      const hasLeft = hasDockedParent || inputs.has(0);
      const hasRight = inputs.has(1);
      return hasLeft && hasRight;
    }
    case 'sort':
      return (
        (node.conditions ?? []).some((c) => c.column !== '') ||
        node.sortColumn !== ''
      );
    case 'limit':
      return node.limitCount !== '' && /^\d+$/.test(node.limitCount);
    case 'groupby': {
      const hasGroup = node.groupColumns.some((c) => c);
      const aggsValid = node.aggregations.every(
        (a) => (!a.alias && !a.column) || a.alias,
      );
      return hasGroup && aggsValid;
    }
    default:
      return true;
  }
}

function isFilterValid(conditions: readonly FilterCondition[]): boolean {
  if (conditions.length === 0) return true;
  return conditions.every((c) => {
    if (!c.column) return true; // Empty conditions are skipped in SQL gen
    if (UNARY_OPS.has(c.op)) return true;
    return c.value !== '';
  });
}

// Helper to find the parent node (node that has this node as nextId)
export function findDockedParent(
  nodes: Map<string, NodeData>,
  nodeId: string,
): NodeData | undefined {
  for (const node of nodes.values()) {
    if (node.nextId === nodeId) {
      return node;
    }
  }
  return undefined;
}

// Helper to find input nodes via connections
export function findConnectedInputs(
  nodes: Map<string, NodeData>,
  connections: Connection[],
  nodeId: string,
): Map<number, NodeData> {
  const inputs = new Map<number, NodeData>();
  for (const conn of connections) {
    if (conn.toNode === nodeId) {
      const inputNode = nodes.get(conn.fromNode);
      if (inputNode) {
        inputs.set(conn.toPort, inputNode);
      }
    }
  }
  return inputs;
}

// Compute the output columns produced by a node, flowing through the chain.
// Returns undefined if columns can't be determined.
export function getOutputColumnsForNode(
  nodes: Map<string, NodeData>,
  connections: Connection[],
  nodeId: string,
  sqlModules: SqlModules | undefined,
): ColumnDef[] | undefined {
  const node = nodes.get(nodeId);
  if (!node) return undefined;

  // Walk the chain from root to this node
  const visited = new Set<string>();
  const order: NodeData[] = [];
  collectUpstream(nodes, connections, nodeId, visited, order);

  let columns: ColumnDef[] | undefined;

  for (const n of order) {
    switch (n.type) {
      case 'from': {
        if (!n.table || !sqlModules) {
          columns = undefined;
        } else {
          const table = sqlModules.getTable(n.table);
          columns = table
            ? table.columns.map((c) => ({name: c.name, type: c.type}))
            : undefined;
        }
        break;
      }
      case 'selection': {
        columns = [
          {name: 'id', type: {kind: 'int'}},
          {name: 'ts', type: {kind: 'timestamp'}},
          {name: 'dur', type: {kind: 'duration'}},
        ];
        break;
      }
      case 'select': {
        // Must match tryFold: only explicitly checked columns are projected.
        const selected = Object.entries(n.columns)
          .filter(([_, checked]) => checked)
          .map(([col]) => columns?.find((c) => c.name === col) ?? {name: col});
        const exprAliases: ColumnDef[] = n.expressions
          .filter((e) => e.alias && e.expression)
          .map((e) => ({name: e.alias}));
        if (selected.length > 0) {
          columns = [...selected, ...exprAliases];
        } else if (exprAliases.length > 0) {
          columns = [...(columns ?? []), ...exprAliases];
        }
        break;
      }
      case 'groupby': {
        const groupCols: ColumnDef[] = n.groupColumns
          .filter((c) => c)
          .map((c) => columns?.find((col) => col.name === c) ?? {name: c});
        const aggAliases: ColumnDef[] = n.aggregations
          .filter((a) => a.alias)
          .map((a) => {
            if (a.func === 'COUNT') {
              return {name: a.alias, type: {kind: 'int' as const}};
            }
            const orig = columns?.find((c) => c.name === a.column);
            return {name: a.alias, type: orig?.type};
          });
        const result = [...groupCols, ...aggAliases];
        if (result.length > 0) {
          columns = result;
        }
        break;
      }
      case 'extend': {
        // All left columns + selected right columns with aliases.
        const leftParent = getPrimaryInput(nodes, connections, n.id);
        const leftAvail = leftParent
          ? getOutputColumnsForNode(
              nodes,
              connections,
              leftParent.id,
              sqlModules,
            )
          : undefined;
        const rightInput = findConnectedInputs(nodes, connections, n.id).get(1);
        const rightAvail = rightInput
          ? getOutputColumnsForNode(
              nodes,
              connections,
              rightInput.id,
              sqlModules,
            )
          : undefined;
        const leftNames = leftAvail?.map((c) => c.name) ?? [];
        const extendAliases = getExtendColumnAliases(n, leftNames);
        const extendResult: ColumnDef[] = [
          ...(leftAvail ?? []),
          ...extendAliases.map((a) => {
            const orig = rightAvail?.find((c) => c.name === a.column);
            return {name: a.alias, type: orig?.type};
          }),
        ];
        columns = extendResult.length > 0 ? extendResult : undefined;
        break;
      }
      case 'extract_arg': {
        const extractAliases: ColumnDef[] = n.extractions
          .filter((e) => e.column && e.argName && e.alias)
          .map((e) => ({name: e.alias}));
        if (extractAliases.length > 0) {
          columns = [...(columns ?? []), ...extractAliases];
        }
        break;
      }
      case 'interval_intersect': {
        columns = getIntervalIntersectOutputColumns(n);
        break;
      }
      case 'union_all': {
        // Output columns come from the left input (SQL UNION ALL uses
        // column names from the first SELECT).
        const leftParent = getPrimaryInput(nodes, connections, n.id);
        if (leftParent) {
          columns =
            getOutputColumnsForNode(
              nodes,
              connections,
              leftParent.id,
              sqlModules,
            ) ?? columns;
        }
        break;
      }
      // filter, sort, limit don't change columns
      default:
        break;
    }
  }

  return columns;
}

// Get the input columns available to a node (i.e. its upstream parent's output).
export function getColumnsForNode(
  nodes: Map<string, NodeData>,
  connections: Connection[],
  nodeId: string,
  sqlModules: SqlModules | undefined,
): ColumnDef[] {
  const parent = getPrimaryInput(nodes, connections, nodeId);
  if (!parent) return [];
  return (
    getOutputColumnsForNode(nodes, connections, parent.id, sqlModules) ?? []
  );
}

// Compute the output columns for an interval_intersect node.
// Output: ts, dur, id_0, id_1, plus partition columns.
export function getIntervalIntersectOutputColumns(
  node: IntervalIntersectNodeData,
): ColumnDef[] {
  // _interval_intersect! outputs exactly these columns:
  // ts, dur, id_0, id_1, plus partition columns.
  // Note: ts_N/dur_N are NOT output by the macro (only id_N are).
  const result: ColumnDef[] = [
    {name: 'ts', type: {kind: 'timestamp'}},
    {name: 'dur', type: {kind: 'duration'}},
    {name: 'id_0', type: {kind: 'int'}},
    {name: 'id_1', type: {kind: 'int'}},
  ];

  // Add selected partition columns.
  for (const col of node.partitionColumns) {
    if (col) {
      result.push({name: col});
    }
  }

  return result;
}

// Get the primary input node for a given node (docked parent or port 0 connection)
export function getPrimaryInput(
  nodes: Map<string, NodeData>,
  connections: Connection[],
  nodeId: string,
): NodeData | undefined {
  const dockedParent = findDockedParent(nodes, nodeId);
  if (dockedParent) return dockedParent;
  const connectedInputs = findConnectedInputs(nodes, connections, nodeId);
  return connectedInputs.get(0);
}

// Collect all upstream nodes in topological order (dependencies first).
export function collectUpstream(
  nodes: Map<string, NodeData>,
  connections: Connection[],
  nodeId: string,
  visited: Set<string>,
  order: NodeData[],
): void {
  if (visited.has(nodeId)) return;
  visited.add(nodeId);

  const node = nodes.get(nodeId);
  if (!node) return;

  // Visit primary input
  const primaryInput = getPrimaryInput(nodes, connections, nodeId);
  if (primaryInput) {
    collectUpstream(nodes, connections, primaryInput.id, visited, order);
  }

  // Visit secondary input (extend/interval_intersect/union_all right side, port 1)
  if (
    node.type === 'extend' ||
    node.type === 'interval_intersect' ||
    node.type === 'union_all'
  ) {
    const connectedInputs = findConnectedInputs(nodes, connections, nodeId);
    const rightInput = connectedInputs.get(1);
    if (rightInput) {
      collectUpstream(nodes, connections, rightInput.id, visited, order);
    }
  }

  order.push(node);
}

// Find root nodes (not referenced by any other node's nextId)
export function getRootNodeIds(nodes: Map<string, NodeData>): string[] {
  const referenced = new Set<string>();
  for (const node of nodes.values()) {
    if (node.nextId) referenced.add(node.nextId);
  }
  return Array.from(nodes.keys()).filter((id) => !referenced.has(id));
}
