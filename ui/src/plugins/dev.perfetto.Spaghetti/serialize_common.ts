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

// Shared pieces of the per-dialect serializers: options and results, the walk
// that finds the nodes to render, and the generator context.

import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import type {Port} from './graph_model';
import {
  type ColumnDef,
  getOutputColumnsForNode,
  type GraphIndex,
  resolveInputs,
  tryGetManifest,
} from './graph_utils';
import type {ConnectedInput, GeneratorContext} from './node_types';

export interface SerializeOptions {
  /**
   * Nodes that already exist as tables, by node id. They are referenced by
   * name and nothing upstream of them is rendered.
   */
  readonly tables?: ReadonlyMap<string, string>;
  readonly sqlModules?: SqlModules;
}

export type SerializeResult =
  | {
      readonly ok: true;
      readonly sql: string;
      /** Stdlib modules the query needs included first. */
      readonly includes: readonly string[];
      /** Set when the query just reads a single named relation. */
      readonly relation?: string;
    }
  | {
      readonly ok: false;
      readonly error: string;
      /** The node that failed, if the failure is down to one node. */
      readonly nodeId?: string;
    };

export type SerializeError = Extract<SerializeResult, {ok: false}>;

/**
 * The nodes to render for `outputId`, inputs first. The walk stops at nodes in
 * `tables`, which are not included.
 */
export function renderOrder(
  index: GraphIndex,
  outputId: string,
  tables: ReadonlyMap<string, string>,
): string[] | SerializeError {
  const order: string[] = [];
  const visiting = new Set<string>();
  const visited = new Set<string>();
  function visit(nodeId: string): SerializeError | undefined {
    if (visited.has(nodeId) || tables.has(nodeId)) return undefined;
    if (visiting.has(nodeId)) {
      return {ok: false, error: 'The graph has a cycle.', nodeId};
    }
    const node = index.nodes[nodeId];
    if (node === undefined) {
      return {ok: false, error: `Node "${nodeId}" does not exist.`, nodeId};
    }
    if (tryGetManifest(node.type) === undefined) {
      return {ok: false, error: `Unknown node type "${node.type}".`, nodeId};
    }
    visiting.add(nodeId);
    for (const inp of resolveInputs(index, nodeId)) {
      const err = visit(inp.upstreamId);
      if (err !== undefined) return err;
    }
    visiting.delete(nodeId);
    visited.add(nodeId);
    order.push(nodeId);
    return undefined;
  }
  const err = visit(outputId);
  return err ?? order;
}

export function makeContext<I>(
  index: GraphIndex,
  inputs: ReadonlyArray<{readonly port: Port; readonly upstreamId: string}>,
  inputOf: (nodeId: string) => I,
  sqlModules: SqlModules | undefined,
): GeneratorContext<I> {
  const connected: Array<ConnectedInput<I>> = inputs.map((inp) => ({
    port: inp.port.name,
    input: inputOf(inp.upstreamId),
  }));
  const byPort = new Map(connected.map((c) => [c.port, c.input]));
  const columnsByPort = new Map<string, ColumnDef[] | undefined>(
    inputs.map((inp) => [
      inp.port.name,
      getOutputColumnsForNode(index, inp.upstreamId, sqlModules),
    ]),
  );
  const ports = inputs.map((inp) => inp.port);
  return {
    inputPorts: ports,
    input: (port) => byPort.get(port),
    inputs: () => connected,
    getInputColumns: (port) => columnsByPort.get(port),
    sqlModules,
  };
}

export function indent(text: string): string {
  return text
    .split('\n')
    .map((line) => (line === '' ? line : `  ${line}`))
    .join('\n');
}
