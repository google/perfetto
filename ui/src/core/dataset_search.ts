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
import {getOrCreate} from '../base/utils';
import {FilterExpression, SearchProvider} from '../public/search';
import {Track} from '../public/track';
import {SourceDataset} from '../trace_processor/dataset';
import {Engine} from '../trace_processor/engine';
import {LONG, NUM, SqlValue, UNKNOWN} from '../trace_processor/query_result';

// Type alias for search results
export type SearchResult = {
  id: number;
  ts: time;
  track: Track;
};

type PartitionMap = Map<string, Map<SqlValue, Track[]>>;

// Defines a group of tracks that use the same dataset source, including a LUT
// to find the corresponding track.
export interface TrackGroup {
  readonly src: string;
  readonly schema: Record<string, SqlValue>;
  readonly nonPartitioned: Track[];
  readonly partitioned: PartitionMap;
}

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
  const trackGroups = buildTrackGroups(tracks);

  const results = await searchTrackGroupsWithFilter(
    engine,
    trackGroups,
    filter,
  );

  return results;
}

function buildTrackGroups(
  tracks: ReadonlyArray<Track>,
): Map<string, TrackGroup> {
  const trackGroups = new Map<string, TrackGroup>();
  for (const track of tracks) {
    const dataset = track.renderer.getDataset?.();
    if (dataset) {
      const src = dataset.src;
      const trackGroup = getOrCreate(trackGroups, src, () => ({
        src,
        schema: {},
        nonPartitioned: [],
        partitioned: new Map(),
      }));
      addTrackToTrackGroup(trackGroup, track, dataset);
    }
  }
  return trackGroups;
}

function addTrackToTrackGroup(
  trackGroup: TrackGroup,
  track: Track,
  dataset: SourceDataset,
): void {
  const filter = dataset.filter;
  const schema = dataset.schema;

  // Combine schemas from all datasets in the group.
  for (const [col, type] of Object.entries(schema)) {
    // TODO(stevegolton): This is a bit of a hack as the data types could
    // conflict. In the future we will probably switch to a centralized
    // datasource approach which tracks will point to that has its own schema.
    trackGroup.schema[col] = type;
  }

  if (filter === undefined) {
    trackGroup.nonPartitioned.push(track);
  } else {
    const partitions = getOrCreate(
      trackGroup.partitioned,
      filter.col,
      () => new Map<SqlValue, Track[]>(),
    );
    const addTrackToPartition = (value: SqlValue) => {
      const key = normalizeMapKey(value);
      const partition = getOrCreate(partitions, key, () => []);
      partition.push(track);
    };

    if ('eq' in filter) {
      addTrackToPartition(filter.eq);
    } else {
      for (const value of filter.in) {
        addTrackToPartition(value);
      }
    }
  }
}

// Normalizes values used as keys in the partition map.
// This is necessary because SQL queries might return integer values as BigInts
// (e.g., for LONG types), while filter definitions might use standard numbers.
// This ensures consistent key types (BigInt for integers, others as-is)
// for reliable lookups in the JavaScript Map.
function normalizeMapKey(value: SqlValue): SqlValue {
  if (typeof value === 'number' && Number.isInteger(value)) {
    return BigInt(value);
  } else {
    return value;
  }
}

async function searchTrackGroupsWithFilter(
  engine: Engine,
  trackGroups: Map<string, TrackGroup>,
  filter: FilterExpression,
): Promise<SearchResult[]> {
  let searchResults: SearchResult[] = [];

  // Process each track group
  for (const trackGroup of trackGroups.values()) {
    // Only search track groups that implement the required schema
    // The schema check ensures 'id', 'ts' columns exist.
    const groupDataset = new SourceDataset({
      src: trackGroup.src,
      schema: trackGroup.schema,
    });
    if (groupDataset.implements({id: NUM, ts: LONG})) {
      const results = await searchTrackGroup(trackGroup, filter, engine);
      searchResults = searchResults.concat(results);
    }
  }

  return searchResults;
}

async function searchTrackGroup(
  trackGroup: TrackGroup,
  filter: FilterExpression,
  engine: Engine,
): Promise<SearchResult[]> {
  const results: SearchResult[] = [];
  const partitionCols = Array.from(trackGroup.partitioned.keys());
  const partitionColSchema = Object.fromEntries(
    partitionCols.map((key) => [key, UNKNOWN]),
  );

  // Ensure required columns plus any partition columns are selected.
  const schema = {id: NUM, ts: LONG, ...partitionColSchema};
  const selectCols = ['id', 'ts', ...partitionCols];

  // Build and execute search query
  const query = `
    SELECT
      ${selectCols.map((c) => `source.${c} AS ${c}`).join()}
    FROM (${trackGroup.src}) AS source
    ${filter.join ? `JOIN ${filter.join}` : ''}
    WHERE ${filter.where}
  `;
  const result = await engine.query(query);

  // Process query results
  for (const iter = result.iter(schema); iter.valid(); iter.next()) {
    const id = iter.id;
    const ts = Time.fromRaw(iter.ts);

    // Add results for matching partitioned tracks
    for (const colName of partitionCols) {
      const partitionValue = iter.get(colName);
      const tracks = trackGroup.partitioned.get(colName)?.get(partitionValue);

      if (tracks) {
        for (const track of tracks) {
          results.push({id, ts, track});
        }
      }
    }

    // Add results for non-partitioned tracks (they match any row from the
    // source)
    for (const track of trackGroup.nonPartitioned) {
      results.push({id, ts, track});
    }
  }

  return results;
}
