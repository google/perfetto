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

import {sqliteString} from '../base/string_utils';
import {exists} from '../base/utils';
import {
  ANDROID_LOGS_TRACK_KIND,
  CPU_SLICE_TRACK_KIND,
} from '../public/track_kinds';
import {Engine} from '../trace_processor/engine';
import {LONG, NUM, STR} from '../trace_processor/query_result';
import {escapeSearchQuery} from '../trace_processor/query_utils';
import {CurrentSearchResults, SearchSource} from './search_data';
import {TrackManagerImpl} from './track_manager';

/**
 * Executes a SQL-based search across the trace data.
 *
 * This function searches for matches in:
 * - Thread and process names (via sched table)
 * - Slice names and arguments
 * - Android log messages
 *
 * @param engine - The trace processor engine
 * @param trackManager - The track manager to resolve track URIs
 * @param searchText - The search term
 * @returns Search results containing matching events
 */
export async function executeSqlSearch(
  engine: Engine,
  trackManager: TrackManagerImpl,
  searchText: string,
): Promise<CurrentSearchResults> {
  // Build indexes for track lookups
  const trackUrisByCpu = new Map<number, string>();
  const allTracks = trackManager.getAllTracks();
  allTracks.forEach((td) => {
    const tags = td?.tags;
    const cpu = tags?.cpu;
    const kinds = tags?.kinds;
    exists(cpu) &&
      kinds?.includes(CPU_SLICE_TRACK_KIND) &&
      trackUrisByCpu.set(cpu, td.uri);
  });

  const trackUrisByTrackId = new Map<number, string>();
  allTracks.forEach((td) => {
    const trackIds = td?.tags?.trackIds ?? [];
    trackIds.forEach((trackId) => trackUrisByTrackId.set(trackId, td.uri));
  });

  const searchLiteral = escapeSearchQuery(searchText);

  // Find matching threads/processes
  const utidRes = await engine.query(`select utid from thread join process
    using(upid) where
      thread.name GLOB ${searchLiteral} or
      process.name GLOB ${searchLiteral}`);
  const utids = [];
  for (const it = utidRes.iter({utid: NUM}); it.valid(); it.next()) {
    utids.push(it.utid);
  }

  // Execute main search query
  const res = await engine.query(`
    select
      id as sliceId,
      ts,
      'cpu' as source,
      cpu as sourceId,
      utid
    from sched where utid in (${utids.join(',')})
    union all
    select *
    from (
      select
        slice_id as sliceId,
        ts,
        'slice' as source,
        track_id as sourceId,
        0 as utid
        from slice
        where slice.name GLOB ${searchLiteral}
          or (
            0 != CAST(${sqliteString(searchText)} AS INT) and
            sliceId = CAST(${sqliteString(searchText)} AS INT)
          )
      union
      select
        slice_id as sliceId,
        ts,
        'slice' as source,
        track_id as sourceId,
        0 as utid
      from slice
      join args using(arg_set_id)
      where string_value GLOB ${searchLiteral} or key GLOB ${searchLiteral}
    )
    union all
    select
      id as sliceId,
      ts,
      'log' as source,
      0 as sourceId,
      utid
    from android_logs where msg GLOB ${searchLiteral}
    order by ts
  `);

  // Process results
  const searchResults: CurrentSearchResults = {
    eventIds: new Float64Array(0),
    tses: new BigInt64Array(0),
    utids: new Float64Array(0),
    sources: [],
    trackUris: [],
    totalResults: 0,
  };

  const rows = res.numRows();
  searchResults.eventIds = new Float64Array(rows);
  searchResults.tses = new BigInt64Array(rows);
  searchResults.utids = new Float64Array(rows);

  const it = res.iter({
    sliceId: NUM,
    ts: LONG,
    source: STR,
    sourceId: NUM,
    utid: NUM,
  });
  for (; it.valid(); it.next()) {
    let track: string | undefined = undefined;

    if (it.source === 'cpu') {
      track = trackUrisByCpu.get(it.sourceId);
    } else if (it.source === 'slice') {
      track = trackUrisByTrackId.get(it.sourceId);
    } else if (it.source === 'log') {
      track = trackManager
        .getAllTracks()
        .find((td) => td.tags?.kinds?.includes(ANDROID_LOGS_TRACK_KIND))?.uri;
    }
    // The .get() calls above could return undefined, this isn't just an else.
    if (track === undefined) {
      continue;
    }

    const i = searchResults.totalResults++;
    searchResults.trackUris.push(track);
    searchResults.sources.push(it.source as SearchSource);
    searchResults.eventIds[i] = it.sliceId;
    searchResults.tses[i] = it.ts;
    searchResults.utids[i] = it.utid;
  }

  return searchResults;
}
