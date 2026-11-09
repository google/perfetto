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

// Runs serialized chunks: builds every chunk the output needs into a
// `CREATE PERFETTO TABLE <hash>` (unless it's already cached), with the
// chunk's includes in the same statement.
//
// Every chunk except the last (the output chunk) is read by name by a later
// chunk, so it must be a table. The output chunk is built only when the caller
// asks for it to be materialized.

import type {Engine} from '../../trace_processor/engine';
import type {ChunkQuery} from './chunk_queries';

/** A materialized chunk: the table named `hash` in TraceProcessor. */
export interface CacheEntry {
  readonly hash: string;
  readonly sql: string;
  readonly createdAt: number;
  readonly materializeTimeMs: number;
  readonly lastHitAt: number;
  readonly hitCount: number;
}

export type StepRunOutcome =
  | {readonly kind: 'building'}
  | {readonly kind: 'hit'; readonly savedTimeMs: number}
  | {readonly kind: 'built'; readonly timeMs: number}
  | {readonly kind: 'skipped'};

export interface ChunkRunHooks {
  /** Called with a fresh outcomes map whenever a chunk's outcome changes. */
  onProgress?(outcomes: ReadonlyMap<string, StepRunOutcome>): void;
  /**
   * Called once a chunk's table is built, before its build time is recorded,
   * so any time spent here counts towards it.
   */
  onChunkEnd?(chunk: ChunkQuery, queryTimeMs: number): Promise<void> | void;
}

export interface ChunkRunResult {
  /** Cache entries for the tables built in this run (even if it failed). */
  readonly built: readonly CacheEntry[];
  /** Hashes of the chunks served from the cache. */
  readonly hits: readonly string[];
  /** Outcome of every chunk, by hash. */
  readonly outcomes: ReadonlyMap<string, StepRunOutcome>;
  /** Hashes of the tables the output query reads. */
  readonly outputReads: readonly string[];
  readonly error?: string;
}

/**
 * Runs `chunks` (in plan order, the output chunk last) against `engine`.
 * `cache` is only read: the caller records `built` and `hits` itself.
 */
export async function runChunks(
  engine: Engine,
  chunks: readonly ChunkQuery[],
  materializeOutput: boolean,
  cache: ReadonlyMap<string, CacheEntry>,
  hooks: ChunkRunHooks = {},
): Promise<ChunkRunResult> {
  const out = chunks[chunks.length - 1];
  if (out === undefined) {
    return {
      built: [],
      hits: [],
      outcomes: new Map(),
      outputReads: [],
      error: 'Empty plan.',
    };
  }
  const outputReads = materializeOutput ? [out.hash] : out.inputs;

  // The chunks the output needs: walk back from what it reads, stopping at
  // chunks already cached (their inputs aren't needed, even if evicted).
  const byHash = new Map(chunks.map((c) => [c.hash, c]));
  const needed = new Set<string>();
  function visit(hash: string): void {
    if (needed.has(hash)) return;
    needed.add(hash);
    if (!cache.has(hash)) byHash.get(hash)?.inputs.forEach(visit);
  }
  outputReads.forEach(visit);

  const built: CacheEntry[] = [];
  const hits: string[] = [];
  let outcomes = new Map<string, StepRunOutcome>(
    chunks
      .filter((c) => !needed.has(c.hash) && c !== out)
      .map((c) => [c.hash, {kind: 'skipped'}]),
  );
  const setOutcome = (hash: string, outcome: StepRunOutcome) => {
    outcomes = new Map(outcomes).set(hash, outcome);
    hooks.onProgress?.(outcomes);
  };
  const result = (rest: Partial<ChunkRunResult>): ChunkRunResult => ({
    built,
    hits,
    outcomes,
    outputReads,
    ...rest,
  });

  try {
    hooks.onProgress?.(outcomes);

    for (const chunk of chunks) {
      if (!needed.has(chunk.hash)) continue;
      const cached = cache.get(chunk.hash);
      if (cached !== undefined) {
        hits.push(chunk.hash);
        setOutcome(chunk.hash, {
          kind: 'hit',
          savedTimeMs: cached.materializeTimeMs,
        });
        continue;
      }

      setOutcome(chunk.hash, {kind: 'building'});
      const start = performance.now();
      await engine.query(
        [
          `DROP TABLE IF EXISTS ${chunk.hash};`,
          ...chunk.includes.map((inc) => `INCLUDE PERFETTO MODULE ${inc};`),
          `CREATE PERFETTO TABLE ${chunk.hash} AS\n${chunk.sql}`,
        ].join('\n'),
      );
      await hooks.onChunkEnd?.(chunk, performance.now() - start);
      const now = performance.now();
      const timeMs = now - start;
      built.push({
        hash: chunk.hash,
        sql: chunk.sql,
        createdAt: now,
        materializeTimeMs: timeMs,
        lastHitAt: now,
        hitCount: 1,
      });
      setOutcome(chunk.hash, {kind: 'built', timeMs});
    }
  } catch (e) {
    return result({error: e instanceof Error ? e.message : String(e)});
  }

  return result({});
}
