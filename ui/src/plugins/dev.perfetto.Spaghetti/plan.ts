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

// Graph -> Plan: the upstream DAG of a target node in topological order, with
// passthrough nodes resolved and incomplete nodes rejected. Plans carry no
// hashes: cache identity is assigned per materialization step from the SQL
// that builds it (see materialization.ts).

import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import type {NodeData, Port} from './graph_model';
import {
  type ColumnDef,
  findConnectedInputs,
  findDockedParent,
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
    const inputColumns = new Map<string, ColumnDef[] | undefined>();
    for (const [i, port] of ports.entries()) {
      const upstream =
        i === 0 && dockedParent ? dockedParent : connected.get(i);
      if (upstream === undefined) continue;
      const upstreamEval = evaluate(upstream.id);
      if (upstreamEval === undefined) return undefined;
      inputs.push({
        port: port.name,
        nodeId: upstreamEval.aliasOf ?? upstreamEval.nodeId,
      });
      inputColumns.set(
        port.name,
        getOutputColumnsForNode(index, upstream.id, sqlModules),
      );
    }

    // Probe the SQL generator to learn whether the node is complete and
    // whether it is a passthrough. The references are placeholders: the real
    // rendering happens in compose.ts.
    const sqlCtx = makeGeneratorContext<SqlInput>(
      ports,
      inputs.map((inp) => ({port: inp.port, input: {ref: inp.nodeId}})),
      inputColumns,
      sqlModules,
    );
    const sqlFrag = manifest.generators.sql(node.config, sqlCtx);
    if (sqlFrag === undefined) return undefined;

    const isError = 'error' in sqlFrag;
    if (!isError && isEmptyFragment(sqlFrag)) {
      // Passthrough: forward the primary input.
      const primary = inputs[0];
      if (primary === undefined) return undefined;
      return {
        nodeId,
        aliasOf: primary.nodeId,
        node,
        manifest,
        ports,
        inputs,
        inputColumns,
        includes: [],
      };
    }

    return {
      nodeId,
      node,
      manifest,
      ports,
      inputs,
      inputColumns,
      includes: isError ? [] : (sqlFrag.includes ?? []),
    };
  }

  return evaluate;
}

function planFrom(
  evaluate: (nodeId: string) => NodeEval | undefined,
  roots: readonly string[],
  targetNodeId: string,
): Plan {
  const order: NodeEval[] = [];
  const visited = new Set<string>();
  function visit(nodeId: string): void {
    if (visited.has(nodeId)) return;
    visited.add(nodeId);
    const e = evaluate(nodeId);
    if (e === undefined) return;
    if (e.aliasOf !== undefined) {
      visit(e.aliasOf);
      return;
    }
    for (const inp of e.inputs) visit(inp.nodeId);
    order.push(e);
  }
  roots.forEach(visit);

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
      expensive: e.manifest.expensive ?? false,
      includes: e.includes,
      refCount: refCounts.get(e.nodeId) ?? 0,
    };
  });

  return {nodes, targetNodeId, includes: [...includes]};
}

/**
 * Builds the plan for `targetId`: its upstream DAG in topological order.
 * Returns undefined if the target or anything upstream of it is invalid or
 * incomplete.
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
  return planFrom(evaluate, [resolvedTarget], resolvedTarget);
}

/**
 * A plan covering every valid node on the canvas (no single target). Used to
 * find every cache table the graph currently uses.
 */
export function buildCanvasPlan(
  index: GraphIndex,
  sqlModules: SqlModules | undefined,
): Plan {
  const evaluate = makeEvaluator(index, sqlModules);
  return planFrom(evaluate, Object.keys(index.nodes), '');
}
