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
import {manifest as filterNode} from './nodes/filter';
import {manifest as fromNode} from './nodes/from';
import {manifest as timeRangeNode} from './nodes/time_range';
import {manifest as selectNode} from './nodes/select';
import {manifest as groupByNode} from './nodes/groupby';
import {manifest as joinNode} from './nodes/join';
import {manifest as extractArgNode} from './nodes/extract_arg';
import {manifest as intervalIntersectNode} from './nodes/interval_intersect';
import {manifest as limitNode} from './nodes/limit';
import {manifest as sortNode} from './nodes/sort';
import {manifest as unionNode} from './nodes/union';
import {NodeData, NodeManifest, ColumnContext} from './node_types';

// Central registry mapping node type strings to their manifests.
const NODE_REGISTRY: Record<string, NodeManifest> = {
  from: fromNode,
  time_range: timeRangeNode,
  select: selectNode,
  filter: filterNode,
  sort: sortNode,
  limit: limitNode,
  groupby: groupByNode,
  join: joinNode,
  extract_arg: extractArgNode,
  interval_intersect: intervalIntersectNode,
  union: unionNode,
};

export function getManifest(type: string): NodeManifest {
  return NODE_REGISTRY[type];
}

// A column definition with optional type information.
export interface ColumnDef {
  readonly name: string;
  readonly type?: PerfettoSqlType;
}

// Context passed to per-node getOutputColumns implementations.
export interface OutputColumnsCtx {
  sqlModules: SqlModules | undefined;
  resolveColumns(nodeId: string): ColumnDef[] | undefined;
  findConnectedInputs(nodeId: string): Map<number, NodeData>;
  getPrimaryInput(nodeId: string): NodeData | undefined;
}

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
// Validates both graph connectivity (all required ports satisfied) and config.
export function isNodeValid(
  node: NodeData,
  nodes: Map<string, NodeData>,
  connections: Connection[],
): boolean {
  const manifest = NODE_REGISTRY[node.type];

  // Generic port validation: every declared input must be satisfied.
  const inputs = manifest.inputs ?? [];
  if (inputs.length > 0) {
    const connected = findConnectedInputs(nodes, connections, node.id);
    const hasDocked = findDockedParent(nodes, node.id) !== undefined;
    for (let i = 0; i < inputs.length; i++) {
      const satisfied = connected.has(i) || (i === 0 && hasDocked);
      if (!satisfied) return false;
    }
  }

  // Config-level validation.
  return manifest.isValid(node.config);
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

  const manifest = NODE_REGISTRY[node.type];
  if (!manifest?.getOutputColumns) return undefined;

  // Build ColumnContext for this node.
  const portInputs = new Map<string, NodeData>();
  const dockedParent = findDockedParent(nodes, nodeId);
  const connected = findConnectedInputs(nodes, connections, nodeId);
  const ports = manifest.inputs ?? [];
  for (let i = 0; i < ports.length; i++) {
    const input = i === 0 && dockedParent ? dockedParent : connected.get(i);
    if (input) portInputs.set(ports[i].name, input);
  }

  const ctx: ColumnContext = {
    getInputColumns(portName: string) {
      const input = portInputs.get(portName);
      if (!input) return undefined;
      return getOutputColumnsForNode(nodes, connections, input.id, sqlModules);
    },
    sqlModules,
  };

  return manifest.getOutputColumns(node.config, ctx);
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

  // Visit all inputs: port 0 can be satisfied by a docked parent,
  // remaining ports use connections.
  const manifest = NODE_REGISTRY[node.type];
  const ports = manifest.inputs ?? [];
  const connected = findConnectedInputs(nodes, connections, nodeId);
  const dockedParent = findDockedParent(nodes, nodeId);
  for (let i = 0; i < ports.length; i++) {
    const input = i === 0 && dockedParent ? dockedParent : connected.get(i);
    if (input) {
      collectUpstream(nodes, connections, input.id, visited, order);
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
