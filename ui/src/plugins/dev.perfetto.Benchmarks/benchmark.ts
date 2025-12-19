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

import {Track} from '../../public/track';
import {SourceDataset, UnionDataset} from '../../trace_processor/dataset';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM, STR_NULL} from '../../trace_processor/query_result';

export type ApproachType =
  | 'uri_string'
  | 'track_index'
  | 'groupid'
  | 'no_lineage';

export interface BenchmarkResult {
  approach: ApproachType;
  trackCount: number;
  queryBuildTimeMs: number;
  queryExecuteTimeMs: number;
  totalTimeMs: number;
  rowCount: number;
}

const SLICELIKE_SPEC = {
  id: NUM,
  name: STR_NULL,
  ts: LONG,
  dur: LONG,
};

const CTE_CHUNK_SIZE = 500;

/**
 * Builds a query from an array of subqueries.
 * If over CTE_CHUNK_SIZE items, breaks into CTEs to help the query parser.
 */
function buildUnionQuery(queries: string[]): string {
  if (queries.length <= CTE_CHUNK_SIZE) {
    return queries.join('\nUNION ALL\n');
  }

  // Break into chunks and create CTEs
  const ctes: string[] = [];
  const cteNames: string[] = [];

  for (let i = 0; i < queries.length; i += CTE_CHUNK_SIZE) {
    const chunk = queries.slice(i, i + CTE_CHUNK_SIZE);
    const cteName = `_chunk_${Math.floor(i / CTE_CHUNK_SIZE)}`;
    cteNames.push(cteName);
    ctes.push(`${cteName} AS (\n${chunk.join('\nUNION ALL\n')}\n)`);
  }

  const cteSection = `WITH ${ctes.join(',\n')}`;
  const finalUnion = cteNames
    .map((name) => `SELECT * FROM ${name}`)
    .join('\nUNION ALL\n');

  return `${cteSection}\n${finalUnion}`;
}

interface TrackGroup {
  groupId: number;
  partitionCol: string;
  tracks: Track[];
  datasets: SourceDataset[];
}

/**
 * Groups tracks by their dataset source, extracting partition info.
 */
function groupTracksBySource(tracks: ReadonlyArray<Track>): TrackGroup[] {
  const sourceGroups = new Map<string, TrackGroup>();
  let nextGroupId = 0;

  for (const track of tracks) {
    const dataset = track.renderer.getDataset?.();
    if (!dataset || !(dataset instanceof SourceDataset)) continue;
    if (!dataset.implements(SLICELIKE_SPEC)) continue;

    let group = sourceGroups.get(dataset.src);
    if (!group) {
      group = {
        groupId: nextGroupId++,
        partitionCol: dataset.filter?.col ?? '',
        tracks: [],
        datasets: [],
      };
      sourceGroups.set(dataset.src, group);
    }
    group.tracks.push(track);
    group.datasets.push(dataset);
  }

  return Array.from(sourceGroups.values());
}

/**
 * Approach 1: URI String Literals
 * Each track's URI injected as a string literal in UNION ALL
 */
async function benchmarkUriString(
  engine: Engine,
  tracks: ReadonlyArray<Track>,
): Promise<BenchmarkResult> {
  const startBuild = performance.now();

  // Build individual queries with URI string literals
  const queries: string[] = [];
  for (const track of tracks) {
    const dataset = track.renderer.getDataset?.();
    if (!dataset || !(dataset instanceof SourceDataset)) continue;
    if (!dataset.implements(SLICELIKE_SPEC)) continue;

    const baseQuery = dataset.query(SLICELIKE_SPEC);
    queries.push(`SELECT *, '${track.uri}' AS track_uri FROM (${baseQuery})`);
  }

  if (queries.length === 0) {
    return {
      approach: 'uri_string',
      trackCount: 0,
      queryBuildTimeMs: 0,
      queryExecuteTimeMs: 0,
      totalTimeMs: 0,
      rowCount: 0,
    };
  }

  const fullQuery = buildUnionQuery(queries);
  const buildTime = performance.now() - startBuild;

  // Execute the query
  const startExecute = performance.now();
  const result = await engine.query(
    `SELECT COUNT(*) as cnt FROM (${fullQuery})`,
  );
  const executeTime = performance.now() - startExecute;

  const rowCount = result.firstRow({cnt: NUM}).cnt;

  return {
    approach: 'uri_string',
    trackCount: tracks.length,
    queryBuildTimeMs: buildTime,
    queryExecuteTimeMs: executeTime,
    totalTimeMs: buildTime + executeTime,
    rowCount,
  };
}

/**
 * Approach 2: Track Index (Integer per track)
 * Each track gets a unique integer index in UNION ALL
 */
async function benchmarkTrackIndex(
  engine: Engine,
  tracks: ReadonlyArray<Track>,
): Promise<BenchmarkResult> {
  const startBuild = performance.now();

  // Build individual queries with integer track index
  const queries: string[] = [];
  let trackIndex = 0;
  for (const track of tracks) {
    const dataset = track.renderer.getDataset?.();
    if (!dataset || !(dataset instanceof SourceDataset)) continue;
    if (!dataset.implements(SLICELIKE_SPEC)) continue;

    const baseQuery = dataset.query(SLICELIKE_SPEC);
    queries.push(
      `SELECT id, name, ts, dur, ${trackIndex} AS track_index FROM (${baseQuery})`,
    );
    trackIndex++;
  }

  if (queries.length === 0) {
    return {
      approach: 'track_index',
      trackCount: 0,
      queryBuildTimeMs: 0,
      queryExecuteTimeMs: 0,
      totalTimeMs: 0,
      rowCount: 0,
    };
  }

  const fullQuery = buildUnionQuery(queries);
  const buildTime = performance.now() - startBuild;

  // Execute the query
  const startExecute = performance.now();
  const result = await engine.query(
    `SELECT COUNT(*) as cnt FROM (${fullQuery})`,
  );
  const executeTime = performance.now() - startExecute;

  const rowCount = result.firstRow({cnt: NUM}).cnt;

  return {
    approach: 'track_index',
    trackCount: tracks.length,
    queryBuildTimeMs: buildTime,
    queryExecuteTimeMs: executeTime,
    totalTimeMs: buildTime + executeTime,
    rowCount,
  };
}

/**
 * Approach 3: GroupID with partition columns
 * Group by source, inject numeric groupid + partition column
 */
async function benchmarkGroupId(
  engine: Engine,
  tracks: ReadonlyArray<Track>,
): Promise<BenchmarkResult> {
  const startBuild = performance.now();

  const groups = groupTracksBySource(tracks);

  if (groups.length === 0) {
    return {
      approach: 'groupid',
      trackCount: 0,
      queryBuildTimeMs: 0,
      queryExecuteTimeMs: 0,
      totalTimeMs: 0,
      rowCount: 0,
    };
  }

  // Build UNION query with groupid column
  // All branches must have the same columns: id, name, ts, dur, groupid, __partition
  const queries: string[] = [];
  for (const group of groups) {
    const union = UnionDataset.create(group.datasets);

    // Build schema that includes the partition column if available
    const querySchema = group.partitionCol
      ? {...SLICELIKE_SPEC, [group.partitionCol]: NUM}
      : SLICELIKE_SPEC;

    const baseQuery = union.query(querySchema);

    // Select partition column if available, otherwise use NULL
    const partitionExpr = group.partitionCol ? group.partitionCol : 'NULL';

    // Explicitly select columns to ensure all branches have the same shape
    queries.push(`
      SELECT id, name, ts, dur, ${group.groupId} AS groupid, ${partitionExpr} AS __partition
      FROM (${baseQuery})
    `);
  }

  const fullQuery = queries.join('\nUNION ALL\n');
  const buildTime = performance.now() - startBuild;

  // Execute the query
  const startExecute = performance.now();
  const result = await engine.query(
    `SELECT COUNT(*) as cnt FROM (${fullQuery})`,
  );
  const executeTime = performance.now() - startExecute;

  const rowCount = result.firstRow({cnt: NUM}).cnt;

  return {
    approach: 'groupid',
    trackCount: tracks.length,
    queryBuildTimeMs: buildTime,
    queryExecuteTimeMs: executeTime,
    totalTimeMs: buildTime + executeTime,
    rowCount,
  };
}

/**
 * Approach 3: No lineage (baseline)
 * Just the basic aggregation without any track info
 */
async function benchmarkNoLineage(
  engine: Engine,
  tracks: ReadonlyArray<Track>,
): Promise<BenchmarkResult> {
  const startBuild = performance.now();

  const datasets: SourceDataset[] = [];
  for (const track of tracks) {
    const dataset = track.renderer.getDataset?.();
    if (!dataset || !(dataset instanceof SourceDataset)) continue;
    if (!dataset.implements(SLICELIKE_SPEC)) continue;
    datasets.push(dataset);
  }

  if (datasets.length === 0) {
    return {
      approach: 'no_lineage',
      trackCount: 0,
      queryBuildTimeMs: 0,
      queryExecuteTimeMs: 0,
      totalTimeMs: 0,
      rowCount: 0,
    };
  }

  const union = UnionDataset.create(datasets);
  const fullQuery = union.query(SLICELIKE_SPEC);
  const buildTime = performance.now() - startBuild;

  // Execute the query
  const startExecute = performance.now();
  const result = await engine.query(
    `SELECT COUNT(*) as cnt FROM (${fullQuery})`,
  );
  const executeTime = performance.now() - startExecute;

  const rowCount = result.firstRow({cnt: NUM}).cnt;

  return {
    approach: 'no_lineage',
    trackCount: tracks.length,
    queryBuildTimeMs: buildTime,
    queryExecuteTimeMs: executeTime,
    totalTimeMs: buildTime + executeTime,
    rowCount,
  };
}

const RUNS_PER_APPROACH = 3;

type BenchmarkFn = (
  engine: Engine,
  tracks: ReadonlyArray<Track>,
) => Promise<BenchmarkResult>;

/**
 * Shuffle an array in place using Fisher-Yates algorithm.
 */
function shuffle<T>(array: T[]): T[] {
  for (let i = array.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [array[i], array[j]] = [array[j], array[i]];
  }
  return array;
}

/**
 * Average multiple benchmark results for the same approach.
 */
function averageResults(results: BenchmarkResult[]): BenchmarkResult {
  const n = results.length;
  if (n === 0) throw new Error('No results to average');

  return {
    approach: results[0].approach,
    trackCount: results[0].trackCount,
    rowCount: results[0].rowCount,
    queryBuildTimeMs:
      results.reduce((sum, r) => sum + r.queryBuildTimeMs, 0) / n,
    queryExecuteTimeMs:
      results.reduce((sum, r) => sum + r.queryExecuteTimeMs, 0) / n,
    totalTimeMs: results.reduce((sum, r) => sum + r.totalTimeMs, 0) / n,
  };
}

/**
 * Run all benchmarks for the given tracks.
 * Each approach is run multiple times in random order, then averaged.
 */
export async function runBenchmarks(
  engine: Engine,
  tracks: ReadonlyArray<Track>,
): Promise<BenchmarkResult[]> {
  // Filter to only slice-like tracks
  const sliceTracks = tracks.filter((t) => {
    const dataset = t.renderer.getDataset?.();
    return dataset?.implements(SLICELIKE_SPEC);
  });

  // Define all benchmark functions
  const benchmarks: BenchmarkFn[] = [
    benchmarkNoLineage,
    benchmarkGroupId,
    benchmarkTrackIndex,
    benchmarkUriString,
  ];

  // Create list of runs: each benchmark repeated RUNS_PER_APPROACH times
  const runs: Array<{fn: BenchmarkFn; index: number}> = [];
  for (const fn of benchmarks) {
    for (let i = 0; i < RUNS_PER_APPROACH; i++) {
      runs.push({fn, index: i});
    }
  }

  // Shuffle to randomize execution order
  shuffle(runs);

  // Run all benchmarks and collect results by approach
  const resultsByApproach = new Map<ApproachType, BenchmarkResult[]>();

  for (const run of runs) {
    const result = await run.fn(engine, sliceTracks);
    const existing = resultsByApproach.get(result.approach) ?? [];
    existing.push(result);
    resultsByApproach.set(result.approach, existing);
  }

  // Average results for each approach in consistent order
  const approachOrder: ApproachType[] = [
    'no_lineage',
    'groupid',
    'track_index',
    'uri_string',
  ];

  const finalResults: BenchmarkResult[] = [];
  for (const approach of approachOrder) {
    const results = resultsByApproach.get(approach);
    if (results && results.length > 0) {
      finalResults.push(averageResults(results));
    }
  }

  return finalResults;
}
