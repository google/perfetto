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

import type {PerfettoSqlType} from '../../trace_processor/perfetto_sql_type';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import {manifest as filterNode} from './nodes/filter';
import {manifest as fromNode} from './nodes/from';
import {manifest as timeRangeNode} from './nodes/time_range';
import {manifest as selectNode} from './nodes/select';
import {manifest as extendNode} from './nodes/extend';
import {manifest as dropNode} from './nodes/drop';
import {manifest as groupByNode} from './nodes/groupby';
import {manifest as joinNode} from './nodes/join';
import {manifest as extractArgNode} from './nodes/extract_arg';
import {manifest as intervalIntersectNode} from './nodes/interval_intersect';
import {manifest as limitNode} from './nodes/limit';
import {manifest as sortNode} from './nodes/sort';
import {manifest as unionNode} from './nodes/union';
import {manifest as chartNode} from './nodes/chart';
import {manifest as sqlNode} from './nodes/sql';
import type {NodeManifest, ColumnContext} from './node_types';
import type {NodeData, RootNodeData, Port} from './graph_model';

// Central registry mapping node type strings to their manifests.
const NODE_REGISTRY: Record<string, NodeManifest> = {
  from: fromNode,
  time_range: timeRangeNode,
  select: selectNode,
  extend: extendNode,
  drop: dropNode,
  filter: filterNode,
  sort: sortNode,
  limit: limitNode,
  groupby: groupByNode,
  join: joinNode,
  extract_arg: extractArgNode,
  interval_intersect: intervalIntersectNode,
  union: unionNode,
  chart: chartNode,
  sql: sqlNode,
};

export function getManifest(type: string): NodeManifest {
  return NODE_REGISTRY[type];
}

// Returns the effective input ports for a node instance.
// Variable-input nodes store their ports in NodeData.inputs; static nodes use
// the manifest's inputs array.
export function getManifestInputs(
  manifest: NodeManifest,
  node: NodeData,
): ReadonlyArray<Port> {
  return manifest.getInputs?.(node.config) ?? [];
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
export function isNodeValid(node: NodeData): boolean {
  const manifest = NODE_REGISTRY[node.type];
  return manifest.isValid(node.config);
}

// Flat index over the graph. Built once from the root node array.
// - nodes: O(1) lookup by node ID.
// - childToParent: O(1) docked-parent lookup (inverse of node.next).
export interface GraphIndex {
  readonly nodes: Record<string, NodeData>;
  readonly childToParent: Map<string, NodeData>;
}

// Flatten a root node array into a GraphIndex.
// Walks every chain link once to build both the node map and the inverse
// childToParent map, so callers never need an O(n) scan for the parent.
export function flattenNodes(roots: RootNodeData[]): GraphIndex {
  const nodes: Record<string, NodeData> = {};
  const childToParent = new Map<string, NodeData>();
  function visit(node: NodeData) {
    nodes[node.id] = node;
    if (node.next) {
      childToParent.set(node.next.id, node);
      visit(node.next);
    }
  }
  for (const root of roots) visit(root);
  return {nodes, childToParent};
}

// Walk a chain to its tail (the last node with no next).
export function chainTail(node: NodeData): NodeData {
  let cur = node;
  while (cur.next) cur = cur.next;
  return cur;
}

// O(1) docked-parent lookup via the pre-built index.
export function findDockedParent(
  index: GraphIndex,
  nodeId: string,
): NodeData | undefined {
  return index.childToParent.get(nodeId);
}

// Helper to find input nodes via the node's inputs array.
export function findConnectedInputs(
  index: GraphIndex,
  nodeId: string,
): Map<number, NodeData> {
  const node = index.nodes[nodeId];
  const result = new Map<number, NodeData>();
  if (!node?.inputs) return result;
  for (let i = 0; i < node.inputs.length; i++) {
    const fromId = node.inputs[i];
    if (fromId !== null && fromId !== undefined) {
      const inputNode = index.nodes[fromId];
      if (inputNode) result.set(i, inputNode);
    }
  }
  return result;
}

// Compute the output columns produced by a node, flowing through the chain.
// Returns undefined if columns can't be determined.
export function getOutputColumnsForNode(
  index: GraphIndex,
  nodeId: string,
  sqlModules: SqlModules | undefined,
): ColumnDef[] | undefined {
  const node = index.nodes[nodeId];
  if (!node) return undefined;

  const manifest = NODE_REGISTRY[node.type];
  if (!manifest?.getOutputColumns) return undefined;

  // Build ColumnContext for this node.
  const portInputs = new Map<string, NodeData>();
  const dockedParent = findDockedParent(index, nodeId);
  const connected = findConnectedInputs(index, nodeId);
  const ports = getManifestInputs(manifest, node);
  for (let i = 0; i < ports.length; i++) {
    const input = i === 0 && dockedParent ? dockedParent : connected.get(i);
    if (input) portInputs.set(ports[i].name, input);
  }

  const ctx: ColumnContext = {
    inputPorts: ports,
    getInputColumns(portName: string) {
      const input = portInputs.get(portName);
      if (!input) return undefined;
      return getOutputColumnsForNode(index, input.id, sqlModules);
    },
    sqlModules,
  };

  return manifest.getOutputColumns(node.config, ctx);
}

// Get the input columns available to a node (i.e. its upstream parent's output).
export function getColumnsForNode(
  index: GraphIndex,
  nodeId: string,
  sqlModules: SqlModules | undefined,
): ColumnDef[] {
  const parent = getPrimaryInput(index, nodeId);
  if (!parent) return [];
  return getOutputColumnsForNode(index, parent.id, sqlModules) ?? [];
}

// Get the primary input node for a given node (docked parent or port 0 connection)
export function getPrimaryInput(
  index: GraphIndex,
  nodeId: string,
): NodeData | undefined {
  const dockedParent = findDockedParent(index, nodeId);
  if (dockedParent) return dockedParent;
  return findConnectedInputs(index, nodeId).get(0);
}

// Collect all upstream nodes in topological order (dependencies first).
export function collectUpstream(
  index: GraphIndex,
  nodeId: string,
  visited: Set<string>,
  order: NodeData[],
): void {
  if (visited.has(nodeId)) return;
  visited.add(nodeId);

  const node = index.nodes[nodeId];
  if (!node) return;

  // Visit all inputs: port 0 can be satisfied by a docked parent,
  // remaining ports use the node's inputs array.
  const manifest = NODE_REGISTRY[node.type];
  const ports = getManifestInputs(manifest, node);
  const connected = findConnectedInputs(index, nodeId);
  const dockedParent = findDockedParent(index, nodeId);
  for (let i = 0; i < ports.length; i++) {
    const input = i === 0 && dockedParent ? dockedParent : connected.get(i);
    if (input) {
      collectUpstream(index, input.id, visited, order);
    }
  }

  order.push(node);
}
