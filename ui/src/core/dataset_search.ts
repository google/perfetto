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
import {Track} from '../public/track';
import {PartitionedDataset, SourceDataset} from '../trace_processor/dataset';
import {Engine} from '../trace_processor/engine';
import {LONG, NUM, SqlValue, STR_NULL} from '../trace_processor/query_result';
import {escapeSearchQuery} from '../trace_processor/query_utils';

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
  readonly nonPartitioned: Track[];
  readonly partitioned: PartitionMap;
}

// Searches for a given searchTerm within all tracks that have a name column.
export async function searchTrackEvents(
  engine: Engine,
  tracks: ReadonlyArray<Track>,
  searchTerm: string,
): Promise<SearchResult[]> {
  const trackGroups = buildTrackGroups(tracks);
  const searchLiteral = escapeSearchQuery(searchTerm);
  // TODO(stevegolton): We currently only search for names but in the future we
  // will allow more search facets to be defined.
  return await searchNames(engine, trackGroups, searchLiteral);
}

export function buildTrackGroups(
  tracks: ReadonlyArray<Track>,
): Map<SourceDataset, TrackGroup> {
  const trackGroups = new Map<SourceDataset, TrackGroup>();
  for (const track of tracks) {
    const dataset = track.track.getDataset?.();
    if (dataset) {
      if (dataset instanceof PartitionedDataset) {
        const base = dataset.base;
        const trackGroup = getOrCreate(trackGroups, base, () => ({
          nonPartitioned: [],
          partitioned: new Map(),
        }));
        addTrackToTrackGroup(trackGroup, track, dataset);
      } else {
        const trackGroup = getOrCreate(trackGroups, dataset, () => ({
          nonPartitioned: [],
          partitioned: new Map(),
        }));
        trackGroup.nonPartitioned.push(track);
      }
    }
  }
  return trackGroups;
}

function addTrackToTrackGroup(
  trackGroup: TrackGroup,
  track: Track,
  dataset: PartitionedDataset,
): void {
  const partition = dataset.partition;

  const partitions = getOrCreate(
    trackGroup.partitioned,
    partition.col,
    () => new Map<SqlValue, Track[]>(),
  );
  const addTrackToPartition = (value: SqlValue) => {
    const partition = getOrCreate(partitions, value, () => []);
    partition.push(track);
  };

  if ('eq' in partition) {
    addTrackToPartition(partition.eq);
  } else {
    for (const value of partition.in) {
      addTrackToPartition(value);
    }
  }
}

async function searchNames(
  engine: Engine,
  trackGroups: Map<SourceDataset, TrackGroup>,
  searchLiteral: string,
): Promise<SearchResult[]> {
  const searchResults: SearchResult[] = [];

  // Process each track group
  for (const [dataset, trackGroup] of trackGroups.entries()) {
    // Only search track groups that implement the required schema
    // The schema check ensures 'id', 'ts', and 'name' columns exist.
    if (dataset.implements({id: NUM, ts: LONG, name: STR_NULL})) {
      const results = await searchTrackGroup(
        engine,
        dataset,
        trackGroup,
        searchLiteral,
      );
      searchResults.push(...results);
    }
  }

  return searchResults;
}

async function searchTrackGroup(
  engine: Engine,
  dataset: SourceDataset,
  trackGroup: TrackGroup,
  searchLiteral: string,
): Promise<SearchResult[]> {
  const results: SearchResult[] = [];
  const partitionCols = Array.from(trackGroup.partitioned.keys());
  const partitionColSchema = Object.fromEntries(
    partitionCols.map((key) => [key, dataset.schema[key]]),
  );

  // Ensure required columns plus any partition columns are selected.
  const schema = {id: NUM, ts: LONG, ...partitionColSchema};
  const selectCols = ['id', 'ts', ...partitionCols];

  // Build and execute search query
  const query = `
    SELECT
      ${selectCols.join(', ')}
    FROM (${dataset.query()})
    WHERE name GLOB ${searchLiteral}
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
