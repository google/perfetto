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

import {Time, time} from '../base/time';
import {FilterExpression, SearchProvider} from '../public/search';
import {Track} from '../public/track';
import {Dataset, UnionDatasetWithLineage} from '../trace_processor/dataset';
import {Engine} from '../trace_processor/engine';
import {LONG, NUM} from '../trace_processor/query_result';

// Type alias for search results
export type SearchResult = {
  id: number;
  ts: time;
  track: Track;
};

// Searches for a given searchTerm within all tracks that have a name column.
export async function searchTrackEvents(
  engine: Engine,
  tracks: ReadonlyArray<Track>,
  providers: ReadonlyArray<SearchProvider>,
  searchTerm: string,
): Promise<SearchResult[]> {
  let results: SearchResult[] = [];

  for (const provider of providers) {
    const filteredTracks = provider.selectTracks(tracks);
    if (filteredTracks.length === 0) {
      // If the provider does not select any tracks, skip it.
      continue;
    }
    const filter = await provider.getSearchFilter(searchTerm);
    if (!filter) {
      // If the provider does not have a filter for this search term, skip it.
      continue;
    }
    const providerResults = await searchTracksUsingProvider(
      engine,
      filteredTracks,
      filter,
    );
    results = results.concat(providerResults);
  }

  // Remove duplicates
  const uniqueResults = new Map<string, SearchResult>();
  for (const result of results) {
    // Use a combination of id and track URI to ensure uniqueness.
    const key = `${result.id}-${result.track.uri}`;
    if (!uniqueResults.has(key)) {
      uniqueResults.set(key, result);
    }
  }

  // Sort the results by timestamp
  const sortedResults = Array.from(uniqueResults.values()).sort((a, b) =>
    Number(a.ts - b.ts),
  );

  return sortedResults;
}

async function searchTracksUsingProvider(
  engine: Engine,
  tracks: ReadonlyArray<Track>,
  filter: FilterExpression,
): Promise<SearchResult[]> {
  // Define schema for search results
  const resultSchema = {id: NUM, ts: LONG};

  // Filter tracks and extract datasets
  const trackDatasetPairs: Array<{track: Track; dataset: Dataset}> = [];
  for (const track of tracks) {
    const dataset = track.renderer.getDataset?.();
    if (dataset?.implements(resultSchema)) {
      trackDatasetPairs.push({track, dataset});
    }
  }

  if (trackDatasetPairs.length === 0) {
    return [];
  }

  // Create union dataset with lineage tracking
  const datasets = trackDatasetPairs.map((p) => p.dataset);
  const unionDataset = UnionDatasetWithLineage.create(datasets);

  // Build query with only the columns we need:
  // - Result columns (id, ts)
  // - Filter columns (needed for WHERE clause)
  // - Lineage columns (__groupid, __partition)
  // This allows the Dataset to optimize away unused columns and joins
  const querySchema = {
    ...resultSchema,
    ...(filter.columns ?? {}),
    __groupid: NUM,
    __partition: NUM,
  };
  const baseQuery = unionDataset.query(querySchema);

  // Select only result columns + lineage columns (not filter columns)
  const resultCols = Object.keys(resultSchema)
    .map((c) => `__root.${c}`)
    .join(',\n');
  const lineageCols = '__root.__groupid,\n__root.__partition';
  let finalQuery: string;
  if (filter.join) {
    finalQuery = `SELECT
${indent(resultCols, 2)},
${indent(lineageCols, 2)}
FROM (
${indent(baseQuery, 2)}
) AS __root
JOIN ${filter.join} WHERE ${filter.where}`;
  } else {
    finalQuery = `SELECT
${indent(resultCols, 2)},
${indent(lineageCols, 2)}
FROM (
${indent(baseQuery, 2)}
) AS __root
WHERE ${filter.where}`;
  }

  // Execute query
  const queryResult = await engine.query(finalQuery);

  // Process results with lineage resolution
  const results: SearchResult[] = [];
  const resultIterSchema = {...resultSchema, __groupid: NUM, __partition: NUM};
  const iter = queryResult.iter(resultIterSchema);
  for (; iter.valid() === true; iter.next()) {
    // Resolve which dataset(s) this row came from
    const sourceDatasets = unionDataset.resolveLineage(iter);

    // Find the corresponding track(s)
    for (const sourceDataset of sourceDatasets) {
      const pair = trackDatasetPairs.find((p) => p.dataset === sourceDataset);
      if (pair) {
        results.push({
          id: iter.get('id') as number,
          ts: Time.fromRaw(iter.get('ts') as bigint),
          track: pair.track,
        });
      }
    }
  }

  return results;
}

function indent(str: string, spaces: number): string {
  const padding = ' '.repeat(spaces);
  return str
    .split('\n')
    .map((line) => padding + line)
    .join('\n');
}
