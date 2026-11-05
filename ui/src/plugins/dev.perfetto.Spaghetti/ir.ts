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

import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import type {NodeData} from './graph_model';
import {
  collectUpstream,
  findConnectedInputs,
  findDockedParent,
  getManifest,
  getManifestInputs,
  getOutputColumnsForNode,
  type GraphIndex,
  isNodeValid,
} from './graph_utils';
import {
  getLogicalOpInputs,
  type LogicalOp,
  type LogicalPlan,
} from './logical_ir';
import type {LogicalOpContext} from './node_types';
import {buildDisplaySql, type IrEntry, lowerToSql} from './sql_backend';

export {buildDisplaySql, lowerToSql};
export type {IrEntry};

// Build a language-agnostic `LogicalPlan` for a specific node's upstream DAG.
// Returns `undefined` if the target node is missing or any upstream node is
// invalid or incomplete.
export function buildLogicalPlan(
  index: GraphIndex,
  nodeId: string,
  sqlModules: SqlModules | undefined,
): LogicalPlan | undefined {
  const targetNode = index.nodes[nodeId];
  if (!targetNode) return undefined;

  const visited = new Set<string>();
  const order: NodeData[] = [];
  collectUpstream(index, nodeId, visited, order);
  if (order.length === 0) return undefined;

  for (const n of order) {
    if (!isNodeValid(n)) return undefined;
  }

  const rawNodes: Array<{readonly nodeId: string; readonly op: LogicalOp}> = [];
  const emittedIds = new Set<string>();
  const refCounts = new Map<string, number>();

  for (const n of order) {
    const manifest = getManifest(n.type);
    const ports = getManifestInputs(manifest, n);
    const dockedParent = findDockedParent(index, n.id);
    const connected = findConnectedInputs(index, n.id);

    const portInputs = new Map<string, NodeData>();
    for (const [i, port] of ports.entries()) {
      const input = i === 0 && dockedParent ? dockedParent : connected.get(i);
      if (input) {
        portInputs.set(port.name, input);
      }
    }

    const ctx: LogicalOpContext = {
      inputPorts: ports,
      sqlModules,
      getInputNodeId(portName: string): string | undefined {
        return portInputs.get(portName)?.id;
      },
      getInputColumns(portName: string) {
        const input = portInputs.get(portName);
        if (!input) return undefined;
        return getOutputColumnsForNode(index, input.id, sqlModules);
      },
    };

    const op = manifest.toLogicalOp(n.config, ctx);
    if (!op) return undefined;

    const inputs = getLogicalOpInputs(op);
    for (const inputId of inputs) {
      if (!emittedIds.has(inputId)) return undefined;
      refCounts.set(inputId, (refCounts.get(inputId) ?? 0) + 1);
    }

    rawNodes.push({nodeId: n.id, op});
    emittedIds.add(n.id);
  }

  return {
    nodes: rawNodes.map(({nodeId: id, op}) => ({
      nodeId: id,
      op,
      refCount: refCounts.get(id) ?? 0,
    })),
    targetNodeId: nodeId,
  };
}

// Build content-addressable IR entries for a specific node's upstream chain.
// Returns entries in topological order, or `undefined` if invalid/incomplete.
export function buildIR(
  index: GraphIndex,
  nodeId: string,
  sqlModules: SqlModules | undefined,
): IrEntry[] | undefined {
  const plan = buildLogicalPlan(index, nodeId, sqlModules);
  if (!plan) return undefined;
  const entries = lowerToSql(plan);
  return entries.length > 0 ? entries : undefined;
}

// Collect all content-addressable IR hashes reachable from any valid node
// currently in the graph. Iterating every node ID (not just chain tails)
// ensures both folded chains and intermediate nodes' hashes are preserved.
export function collectLiveHashes(
  index: GraphIndex,
  sqlModules: SqlModules | undefined,
): Set<string> {
  const live = new Set<string>();
  for (const nodeId of Object.keys(index.nodes)) {
    const entries = buildIR(index, nodeId, sqlModules);
    if (!entries) continue;
    for (const e of entries) {
      live.add(e.hash);
    }
  }
  return live;
}
