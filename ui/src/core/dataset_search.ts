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

import {assertExists} from '../base/logging';
import {Time, time} from '../base/time';
import {FilterExpression, SearchProvider} from '../public/search';
import {Track} from '../public/track';
import {planQuery} from '../trace_processor/dataset_query_utils';
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

  const filteredTracks = tracks.filter((track) => {
    const dataset = track.renderer.getDataset?.();
    return dataset?.implements(resultSchema);
  });

  // Query all tracks with lineage tracking
  const plan = planQuery({
    inputs: filteredTracks,
    datasetFetcher: (track) => assertExists(track.renderer.getDataset?.()),
    columns: resultSchema,
    // Include filter columns - these are needed for the WHERE clause
    // but won't be in the final result
    filterColumns: filter.columns,
    // Skip partition filters since we're searching across all tracks
    skipPartitionFilters: true,
    queryBuilder: (baseQuery: string, resultCols: string[]) => {
      // Select only the result columns, apply JOIN/WHERE filter
      const cols = resultCols.map((c) => `__root.${c}`).join(', ');
      if (filter.join) {
        return `SELECT ${cols} FROM (${baseQuery}) AS __root JOIN ${filter.join} WHERE ${filter.where}`;
      }
      return `SELECT ${cols} FROM (${baseQuery}) AS __root WHERE ${filter.where}`;
    },
  });

  // Execute the query plan, returning results with lineage tracking
  const results = await plan.execute(engine);

  // Map results to SearchResult format
  return results.map((result) => ({
    id: result.row.id,
    ts: Time.fromRaw(result.row.ts),
    track: result.source,
  }));
}
