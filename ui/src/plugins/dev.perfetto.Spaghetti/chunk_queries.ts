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

// Serializes every chunk of a chunk plan to SQL and names each one by a
// Merkle hash. Chunks are serialized inputs-first, with each input read from
// the table named by its own hash, so a change anywhere upstream renames an
// input and so changes every hash downstream of it.
//
// Pure: no engine, no cache. Deciding what to build and running it is up to
// the materializer.

import {ensureDefined} from '../../base/assert';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import {fnvHash, type GraphIndex, tryGetManifest} from './graph_utils';
import type {ChunkPlan} from './plan_chunks';
import {serialize} from './serialize';
import type {SerializeResult} from './serialize_common';
import type {SqlDialect} from './sql_types';

export interface ChunkQuery {
  /** The node whose output this chunk produces. */
  readonly rootId: string;
  /** Table name: `_qb_` plus a hash of the dialect, `includes` and `sql`. */
  readonly hash: string;
  /** The query body, reading its inputs from their tables. */
  readonly sql: string;
  /** Stdlib modules the query needs included first. */
  readonly includes: readonly string[];
  /** Hashes of the chunks this chunk reads, i.e. the tables in `sql`. */
  readonly inputs: readonly string[];
  /** Set when `sql` just reads a single named relation. */
  readonly relation?: string;
}

export type ChunkQueriesResult =
  | {
      readonly ok: true;
      /** Same order as the plan: inputs first, the output chunk last. */
      readonly chunks: readonly ChunkQuery[];
    }
  | {
      readonly ok: false;
      readonly error: string;
      readonly nodeId?: string;
    };

export function serializeChunks(
  index: GraphIndex,
  plan: ChunkPlan,
  options: {
    readonly sqlModules?: SqlModules;
    readonly dialect?: SqlDialect;
  } = {},
): ChunkQueriesResult {
  const dialect = options.dialect ?? 'sql';
  const hashOf = new Map<string, string>(); // chunk rootId -> hash
  const chunks: ChunkQuery[] = [];
  for (const chunk of plan.chunks) {
    // Plan order is inputs first, so every input already has a hash.
    const tables = new Map(
      chunk.inputs.map((id) => [id, ensureDefined(hashOf.get(id))]),
    );
    const inputs = [...tables.values()];
    const res = serialize(index, chunk.rootId, dialect, {
      tables,
      sqlModules: options.sqlModules,
    });
    if (!res.ok) return res;
    // The dialect is part of the hash, so the dialects never share tables.
    const text = [
      dialect,
      ...res.includes.map((inc) => `INCLUDE ${inc};`),
      res.sql,
    ];
    const hash = `_qb_${fnvHash(text.join('\n'))}`;
    hashOf.set(chunk.rootId, hash);
    chunks.push({
      rootId: chunk.rootId,
      hash,
      sql: res.sql,
      includes: res.includes,
      inputs,
      relation: res.relation,
    });
  }
  return {ok: true, chunks};
}

/**
 * Whether the output chunk is materialized: only an expensive output is worth
 * building into a table. A cheap one is read straight from its inputs' tables.
 */
export function isOutputMaterialized(
  index: GraphIndex,
  plan: ChunkPlan,
): boolean {
  const outputNode = index.nodes[plan.outputId];
  return (
    outputNode !== undefined &&
    (tryGetManifest(outputNode.type)?.expensive ?? false)
  );
}

/**
 * The query that reads the output, in `dialect`: from the output chunk's own
 * table when it's materialized, otherwise the output chunk's query over its
 * inputs' tables.
 */
export function outputQuery(
  index: GraphIndex,
  plan: ChunkPlan,
  chunks: readonly ChunkQuery[],
  dialect: SqlDialect,
  options: {readonly sqlModules?: SqlModules} = {},
): SerializeResult {
  // Chunks line up with the plan's, which are in the same order.
  const hashOf = new Map(
    plan.chunks.map((c, i) => [c.rootId, ensureDefined(chunks[i]).hash]),
  );
  const outChunk = ensureDefined(plan.chunks[plan.chunks.length - 1]);
  const tableIds = isOutputMaterialized(index, plan)
    ? [plan.outputId]
    : outChunk.inputs;
  const tables = new Map(
    tableIds.map((id) => [id, ensureDefined(hashOf.get(id))]),
  );
  return serialize(index, plan.outputId, dialect, {
    tables,
    sqlModules: options.sqlModules,
  });
}
