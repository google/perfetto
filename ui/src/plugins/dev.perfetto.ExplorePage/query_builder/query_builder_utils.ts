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

import protos from '../../../protos';
import {QueryResponse} from '../../../components/query_table/queries';
import {Engine} from '../../../trace_processor/engine';
import {stringifyJsonWithBigints} from '../../../base/json_utils';
import {uuidv4Sql} from '../../../base/uuid';
import {Query, QueryNode} from '../query_node';
import {SqlSourceNode} from './nodes/sources/sql_source';

export function findErrors(
  query?: Query | Error,
  response?: QueryResponse,
): Error | undefined {
  if (query instanceof Error) {
    return query;
  }
  if (response?.error) {
    return new Error(response.error);
  }
  return undefined;
}

export function findWarnings(
  response: QueryResponse | undefined,
  node: QueryNode,
): Error | undefined {
  if (!response || response.error) {
    return undefined;
  }

  if (
    response.statementCount > 0 &&
    response.statementWithOutputCount === 0 &&
    response.columns.length === 0
  ) {
    return new Error('The last statement must produce an output.');
  }

  if (node instanceof SqlSourceNode && response.statementCount > 1) {
    const statements = response.query
      .split(';')
      .map((x) => x.trim())
      .filter((x) => x.length > 0);
    const allButLast = statements.slice(0, statements.length - 1);
    const moduleIncludeRegex = /^\s*INCLUDE\s+PERFETTO\s+MODULE\s+[\w._]+\s*$/i;
    for (const stmt of allButLast) {
      if (!moduleIncludeRegex.test(stmt)) {
        return new Error(
          `Only 'INCLUDE PERFETTO MODULE ...;' statements are ` +
            `allowed before the final statement. Error on: "${stmt}"`,
        );
      }
    }
  }

  return undefined;
}

// ============================================================================
// Query Analysis Utilities
// ============================================================================

// Builds structured queries via DFS post-order traversal (dependency order).
export function getStructuredQueries(
  finalNode: QueryNode,
): protos.PerfettoSqlStructuredQuery[] | Error {
  if (finalNode.finalCols === undefined) {
    return new Error(
      `Cannot get structured queries: node ${finalNode.nodeId} has no finalCols`,
    );
  }

  // Use DFS post-order traversal to ensure dependencies come before dependents
  const visited = new Set<string>();
  const orderedNodes: QueryNode[] = [];

  // Recursive DFS helper that adds nodes in post-order (children before parents)
  function dfsPostOrder(node: QueryNode): Error | undefined {
    if (visited.has(node.nodeId)) {
      return undefined;
    }
    visited.add(node.nodeId);

    // Validate the node
    if (!node.validate()) {
      return new Error(
        `Cannot get structured queries: node ${node.nodeId} failed validation`,
      );
    }

    // Visit all inputs first (primary and secondary)
    const inputs: QueryNode[] = [];
    if (node.primaryInput) {
      inputs.push(node.primaryInput);
    }
    if (node.secondaryInputs) {
      for (const [, inputNode] of node.secondaryInputs.connections) {
        inputs.push(inputNode);
      }
    }

    for (const inputNode of inputs) {
      const error = dfsPostOrder(inputNode);
      if (error) return error;
    }

    // Add this node after all its inputs (post-order)
    orderedNodes.push(node);
    return undefined;
  }

  const error = dfsPostOrder(finalNode);
  if (error) return error;

  // Build structured queries from the ordered nodes (already in dependency order)
  const structuredQueries: protos.PerfettoSqlStructuredQuery[] = [];
  for (const node of orderedNodes) {
    const sq = node.getStructuredQuery();
    if (sq === undefined) {
      return new Error(
        `Cannot get structured queries: node ${node.nodeId} returned undefined from getStructuredQuery()`,
      );
    }
    structuredQueries.push(sq);
  }

  return structuredQueries;
}

// Returns the SQL string from a Query (modules/preambles are baked in by TP).
export function queryToRun(query?: Query): string {
  return query?.sql ?? 'N/A';
}

// Computes a hash of a node's structured query for change detection.
export function hashNodeQuery(node: QueryNode): string | Error {
  const sq = node.getStructuredQuery();
  if (sq === undefined) {
    return new Error(
      `Cannot hash node query: node ${node.nodeId} returned undefined from getStructuredQuery()`,
    );
  }

  // stringifyJsonWithBigints on the protobuf object gives us a stable representation
  // of all the query structure (filters, aggregations, joins, etc.).
  // Protobuf objects have stable field ordering, making this deterministic.
  // Uses bigint-safe stringify to handle bigint values correctly.
  return stringifyJsonWithBigints(sq);
}

// Server-generated summarizer ID for analyzeNode operations.
// This is module-level state that persists across the session.
// Call resetAnalyzeNodeSummarizer() when loading a new trace to clear stale state.
let analyzeNodeSummarizerId: string | undefined = undefined;

/**
 * Resets the analyzeNode summarizer ID. Must be called when loading a new trace
 * to ensure stale summarizer IDs from previous traces are not reused.
 */
export function resetAnalyzeNodeSummarizer(): void {
  analyzeNodeSummarizerId = undefined;
}

// Analyzes a node's query via sync + fetch, returns generated SQL.
export async function analyzeNode(
  node: QueryNode,
  engine: Engine,
): Promise<Query | Error> {
  const structuredQueries = getStructuredQueries(node);
  if (structuredQueries instanceof Error) {
    return structuredQueries;
  }

  if (structuredQueries.length === 0) {
    return new Error('No structured queries to analyze');
  }

  // Build a TraceSummarySpec containing all the queries
  const spec = new protos.TraceSummarySpec();
  spec.query = structuredQueries;

  // Use the node's ID as the query ID. The node's ID is set as the query's id
  // field when the node builds its structured query.
  const queryId = node.nodeId;

  // Create the summarizer if it doesn't exist yet
  if (analyzeNodeSummarizerId === undefined) {
    const newId = `analyze_summarizer_${uuidv4Sql()}`;
    const createRes = await engine.createSummarizer(newId);
    if (
      createRes.error !== undefined &&
      createRes.error !== null &&
      createRes.error !== ''
    ) {
      return new Error(createRes.error);
    }
    analyzeNodeSummarizerId = newId;
  }

  // Update the spec with our queries
  const updateRes = await engine.updateSummarizerSpec(
    analyzeNodeSummarizerId,
    spec,
  );
  if (
    updateRes.error !== undefined &&
    updateRes.error !== null &&
    updateRes.error !== ''
  ) {
    return new Error(updateRes.error);
  }

  // Query the summarizer for this node (materializes on demand)
  const res = await engine.querySummarizer(analyzeNodeSummarizerId, queryId);
  if (!res.exists) {
    return new Error(
      `Query '${queryId}' does not exist after updateSummarizerSpec`,
    );
  }
  if (res.error !== undefined && res.error !== null && res.error !== '') {
    return new Error(res.error);
  }
  if (res.sql === null || res.sql === undefined || res.sql === '') {
    return new Error(
      `analyzeNode: engine returned no SQL for node ${node.nodeId}`,
    );
  }

  return {
    sql: res.sql,
    textproto: res.textproto ?? '',
    standaloneSql: res.standaloneSql ?? '',
  };
}

// Type guard for valid Query object.
export function isAQuery(
  maybeQuery: Query | undefined | Error,
): maybeQuery is Query {
  return (
    maybeQuery !== undefined &&
    maybeQuery !== null &&
    !(maybeQuery instanceof Error) &&
    maybeQuery.sql !== undefined
  );
}
