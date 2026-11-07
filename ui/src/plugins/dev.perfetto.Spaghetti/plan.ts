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

// Graph -> Plan: evaluates each node's generators to derive a dialect-agnostic
// Merkle hash per node, resolves passthrough nodes, and collects the upstream
// DAG of a target node in topological order.

import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import type {NodeData, Port} from './graph_model';
import {
  type ColumnDef,
  findConnectedInputs,
  findDockedParent,
  fnvHash,
  getManifestInputs,
  getOutputColumnsForNode,
  type GraphIndex,
  tryGetManifest,
} from './graph_utils';
import type {
  ConnectedInput,
  GeneratorContext,
  NodeManifest,
  PsnFragment,
  PsnInput,
  SqlFragment,
  SqlInput,
} from './node_types';

export interface PlanInput {
  readonly port: string;
  /** Upstream node id, resolved through passthrough nodes. */
  readonly nodeId: string;
}

export interface PlanNode {
  readonly nodeId: string;
  readonly node: NodeData;
  readonly manifest: NodeManifest;
  readonly ports: ReadonlyArray<Port>;
  /** Connected inputs in port order. The first is the primary input. */
  readonly inputs: readonly PlanInput[];
  /** Output columns of each connected input port. */
  readonly inputColumns: ReadonlyMap<string, ColumnDef[] | undefined>;
  readonly hash: string;
  readonly expensive: boolean;
  readonly includes: readonly string[];
  /** Number of references to this node from other nodes in the plan. */
  readonly refCount: number;
}

export interface Plan {
  /** Topological order; the target is last. Passthrough nodes are removed. */
  readonly nodes: readonly PlanNode[];
  /** The target, resolved through passthrough nodes. */
  readonly targetNodeId: string;
  readonly includes: readonly string[];
}

interface NodeEval {
  readonly nodeId: string;
  readonly hash: string;
  /** Set when this node is a passthrough: the node it forwards. */
  readonly aliasOf?: string;
  readonly node: NodeData;
  readonly manifest: NodeManifest;
  readonly ports: ReadonlyArray<Port>;
  readonly inputs: readonly PlanInput[];
  readonly inputColumns: ReadonlyMap<string, ColumnDef[] | undefined>;
  readonly includes: readonly string[];
}

/** Builds a generator context over a fixed set of connected inputs. */
export function makeGeneratorContext<I>(
  ports: ReadonlyArray<Port>,
  inputs: ReadonlyArray<ConnectedInput<I>>,
  inputColumns: ReadonlyMap<string, ColumnDef[] | undefined>,
  sqlModules: SqlModules | undefined,
): GeneratorContext<I> {
  const byPort = new Map(inputs.map((i) => [i.port, i.input]));
  return {
    inputPorts: ports,
    sqlModules,
    input: (port) => byPort.get(port),
    inputs: () => inputs,
    getInputColumns: (port) => inputColumns.get(port),
  };
}

export function isEmptyFragment(frag: SqlFragment | PsnFragment): boolean {
  return Object.entries(frag).every(
    ([key, value]) => key === 'includes' || value === undefined,
  );
}

function hashOf(value: unknown): string {
  return `_qb_${fnvHash(JSON.stringify(value))}`;
}

/**
 * Evaluates every node reachable on demand, memoized. Invalid / incomplete
 * nodes (and anything downstream of them) evaluate to undefined.
 */
function makeEvaluator(index: GraphIndex, sqlModules: SqlModules | undefined) {
  const memo = new Map<string, NodeEval | undefined>();
  const visiting = new Set<string>();

  function evaluate(nodeId: string): NodeEval | undefined {
    if (memo.has(nodeId)) return memo.get(nodeId);
    if (visiting.has(nodeId)) return undefined; // cycle
    visiting.add(nodeId);
    const result = evaluateNode(nodeId);
    visiting.delete(nodeId);
    memo.set(nodeId, result);
    return result;
  }

  function evaluateNode(nodeId: string): NodeEval | undefined {
    const node = index.nodes[nodeId];
    if (node === undefined) return undefined;
    const manifest = tryGetManifest(node.type);
    if (manifest === undefined || !manifest.isValid(node.config)) {
      return undefined;
    }

    const ports = getManifestInputs(manifest, node);
    const dockedParent = findDockedParent(index, nodeId);
    const connected = findConnectedInputs(index, nodeId);
    const inputs: PlanInput[] = [];
    const hashes = new Map<string, string>();
    const inputColumns = new Map<string, ColumnDef[] | undefined>();
    for (const [i, port] of ports.entries()) {
      const upstream = i === 0 && dockedParent ? dockedParent : connected.get(i);
      if (upstream === undefined) continue;
      const upstreamEval = evaluate(upstream.id);
      if (upstreamEval === undefined) return undefined;
      inputs.push({
        port: port.name,
        nodeId: upstreamEval.aliasOf ?? upstreamEval.nodeId,
      });
      hashes.set(port.name, upstreamEval.hash);
      inputColumns.set(
        port.name,
        getOutputColumnsForNode(index, upstream.id, sqlModules),
      );
    }

    // Render with each input referred to by its hash, so the result (and so
    // the node's hash) is independent of node ids and dialect. Input hashes
    // are also hashed explicitly: clause fragments (e.g. `{where}`) never
    // mention their primary input.
    const inputHashes = inputs.map((inp) => [inp.port, hashes.get(inp.port)!]);
    const sqlCtx = makeGeneratorContext<SqlInput>(
      ports,
      inputs.map((inp) => ({
        port: inp.port,
        input: {ref: hashes.get(inp.port)!},
      })),
      inputColumns,
      sqlModules,
    );
    const sqlFrag = manifest.generators.sql(node.config, sqlCtx);
    if (sqlFrag === undefined) return undefined;

    let hash: string;
    let includes: readonly string[] = [];
    if ('error' in sqlFrag) {
      // Not expressible in Standard SQL: identify it by its PSN rendering.
      const psnCtx = makeGeneratorContext<PsnInput>(
        ports,
        inputs.map((inp) => {
          const h = hashes.get(inp.port)!;
          return {
            port: inp.port,
            input: {
              source: h,
              withStages: (stages) =>
                stages.length > 0 ? `(FROM ${h} |> ${stages.join(' |> ')})` : h,
            },
          };
        }),
        inputColumns,
        sqlModules,
      );
      const psnFrag = manifest.generators.psn?.(node.config, psnCtx);
      if (psnFrag === undefined) return undefined;
      hash = hashOf({type: node.type, inputs: inputHashes, psn: psnFrag});
      if (!('error' in psnFrag)) includes = psnFrag.includes ?? [];
    } else if (isEmptyFragment(sqlFrag)) {
      // Passthrough: forward the primary input.
      const primary = inputs[0];
      if (primary === undefined) return undefined;
      return {
        nodeId,
        hash: hashes.get(primary.port)!,
        aliasOf: primary.nodeId,
        node,
        manifest,
        ports,
        inputs,
        inputColumns,
        includes: [],
      };
    } else {
      hash = hashOf({type: node.type, inputs: inputHashes, sql: sqlFrag});
      includes = sqlFrag.includes ?? [];
    }

    return {
      nodeId,
      hash,
      node,
      manifest,
      ports,
      inputs,
      inputColumns,
      includes,
    };
  }

  return evaluate;
}

/**
 * Builds the plan for `targetId`: its upstream DAG in topological order with
 * a Merkle hash per node. Returns undefined if the target or anything upstream
 * of it is invalid or incomplete.
 */
export function buildPlan(
  index: GraphIndex,
  targetId: string,
  sqlModules: SqlModules | undefined,
): Plan | undefined {
  const evaluate = makeEvaluator(index, sqlModules);
  const target = evaluate(targetId);
  if (target === undefined) return undefined;
  const resolvedTarget = target.aliasOf ?? target.nodeId;

  const order: NodeEval[] = [];
  const visited = new Set<string>();
  function visit(nodeId: string): void {
    if (visited.has(nodeId)) return;
    visited.add(nodeId);
    const e = evaluate(nodeId);
    if (e === undefined) return;
    for (const inp of e.inputs) visit(inp.nodeId);
    order.push(e);
  }
  visit(resolvedTarget);

  const refCounts = new Map<string, number>();
  for (const e of order) {
    for (const inp of e.inputs) {
      refCounts.set(inp.nodeId, (refCounts.get(inp.nodeId) ?? 0) + 1);
    }
  }

  const includes = new Set<string>();
  const nodes: PlanNode[] = order.map((e) => {
    for (const inc of e.includes) includes.add(inc);
    return {
      nodeId: e.nodeId,
      node: e.node,
      manifest: e.manifest,
      ports: e.ports,
      inputs: e.inputs,
      inputColumns: e.inputColumns,
      hash: e.hash,
      expensive: e.manifest.expensive ?? false,
      includes: e.includes,
      refCount: refCounts.get(e.nodeId) ?? 0,
    };
  });

  return {nodes, targetNodeId: resolvedTarget, includes: [...includes]};
}

/**
 * Hashes of every valid expensive node on the canvas: the only nodes whose
 * output is ever materialized, and so the cache tables that must not be
 * evicted while the graph still uses them.
 */
export function collectLiveHashes(
  index: GraphIndex,
  sqlModules: SqlModules | undefined,
): Set<string> {
  const evaluate = makeEvaluator(index, sqlModules);
  const live = new Set<string>();
  for (const nodeId of Object.keys(index.nodes)) {
    const e = evaluate(nodeId);
    if (e !== undefined && e.aliasOf === undefined && e.manifest.expensive) {
      live.add(e.hash);
    }
  }
  return live;
}
