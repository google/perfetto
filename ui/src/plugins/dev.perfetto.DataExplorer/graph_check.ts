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

// Runs a Data Explorer graph against the trace engine purely to surface errors,
// so a caller (e.g. the Intelletto assistant) can check that a graph it built
// actually works and iterate until it is clean. Mirrors what
// QueryExecutionService does for the live UI, but as a one-shot, side-effect-
// free pass: it spins up a throwaway "summarizer", analyses every node's
// structured query, materialises each node to catch runtime-only errors, then
// tears the summarizer down again.

import protos from '../../protos';
import {uuidv4Sql} from '../../base/uuid';
import {getErrorMessage} from '../../base/errors';
import type {Engine} from '../../trace_processor/engine';
import type {QueryNode} from './query_node';

export interface GraphNodeError {
  // The id of the offending node, or a synthetic marker like "(engine)" for
  // failures that aren't attributable to a single node.
  readonly nodeId: string;
  // The node's human title (e.g. "Sql source"), for friendlier messages.
  readonly title: string;
  readonly error: string;
}

function nonEmpty(s: string | undefined | null): string | undefined {
  return s !== undefined && s !== null && s !== '' ? s : undefined;
}

/**
 * Checks every node in `nodes` against the engine and returns one error per
 * failing node (validation, query analysis, or materialisation). An empty array
 * means the whole graph runs cleanly.
 */
export async function collectGraphErrors(
  engine: Engine,
  nodes: ReadonlyArray<QueryNode>,
): Promise<GraphNodeError[]> {
  // First error wins per node, in the order: client-side validation, then
  // server-side analysis, then materialisation - the earliest is the most
  // actionable.
  const errors = new Map<string, GraphNodeError>();
  const add = (nodeId: string, title: string, error: string) => {
    if (!errors.has(nodeId)) errors.set(nodeId, {nodeId, title, error});
  };
  const titleOf = (nodeId: string) =>
    nodes.find((n) => n.nodeId === nodeId)?.getTitle() ?? nodeId;

  // Client-side validation, and collect the structured queries for the rest.
  const structuredQueries: protos.PerfettoSqlStructuredQuery[] = [];
  for (const node of nodes) {
    if (!node.validate()) {
      add(
        node.nodeId,
        node.getTitle(),
        node.context.issues?.queryError?.message ??
          'invalid configuration or missing input',
      );
      continue;
    }
    const sq = node.getStructuredQuery();
    if (sq !== undefined) structuredQueries.push(sq);
  }

  if (structuredQueries.length === 0) {
    return [...errors.values()];
  }

  const summarizerId = `assistant_check_${uuidv4Sql()}`;
  const created = await engine.createSummarizer(summarizerId);
  if (nonEmpty(created.error) !== undefined) {
    add('(engine)', 'engine', created.error!);
    return [...errors.values()];
  }

  try {
    // Analysis pass: catches bad columns/tables and malformed queries with
    // per-node attribution.
    const spec = new protos.TraceSummarySpec();
    spec.query = structuredQueries;
    const updated = await engine.updateSummarizerSpec(summarizerId, spec);
    if (nonEmpty(updated.error) !== undefined) {
      add('(graph)', 'graph', updated.error!);
    }
    for (const q of updated.queries ?? []) {
      const err = nonEmpty(q.error);
      if (err !== undefined && nonEmpty(q.queryId) !== undefined) {
        add(q.queryId!, titleOf(q.queryId!), err);
      }
    }

    // Materialisation pass: surfaces runtime errors that only appear when the
    // query actually runs.
    for (const node of nodes) {
      const res = await engine.querySummarizer(summarizerId, node.nodeId);
      const err = nonEmpty(res.error);
      if (err !== undefined) {
        add(node.nodeId, node.getTitle(), err);
      }
    }
  } catch (e) {
    add('(engine)', 'engine', getErrorMessage(e));
  } finally {
    // Drop everything we materialised so the check leaves no trace behind.
    await engine
      .updateSummarizerSpec(summarizerId, new protos.TraceSummarySpec())
      .catch(() => {});
  }

  return [...errors.values()];
}

/** Formats graph errors into a compact string for a tool result. */
export function formatGraphErrors(
  errors: ReadonlyArray<GraphNodeError>,
): string {
  return errors
    .map((e) => `- node ${e.nodeId} (${e.title}): ${e.error}`)
    .join('\n');
}
