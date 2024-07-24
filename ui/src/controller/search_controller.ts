// Copyright (C) 2019 The Android Open Source Project
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
import {CurrentSearchResults, SearchSource} from '../common/search_data';
import {OmniboxState} from '../common/state';
import {CPU_SLICE_TRACK_KIND} from '../core/track_kinds';
import {globals} from '../frontend/globals';
import {publishSearchResult} from '../frontend/publish';
import {Engine} from '../trace_processor/engine';
import {LONG, NUM, STR} from '../trace_processor/query_result';
import {escapeSearchQuery} from '../trace_processor/query_utils';

import {Controller} from './controller';

export interface SearchControllerArgs {
  engine: Engine;
}

export class SearchController extends Controller<'main'> {
  private engine: Engine;
  private previousOmniboxState?: OmniboxState;
  private updateInProgress: boolean;

  constructor(args: SearchControllerArgs) {
    super('main');
    this.engine = args.engine;
    this.updateInProgress = false;
  }

  run() {
    if (this.updateInProgress) {
      return;
    }

    const omniboxState = globals.state.omniboxState;
    if (omniboxState === undefined || omniboxState.mode === 'COMMAND') {
      return;
    }
    const newOmniboxState = omniboxState;
    if (this.previousOmniboxState === newOmniboxState) {
      return;
    }

    // TODO(hjd): We should restrict this to the start of the trace but
    // that is not easily available here.
    // N.B. Timestamps can be negative.
    this.previousOmniboxState = newOmniboxState;
    const search = newOmniboxState.omnibox;
    if (search === '' || (search.length < 4 && !newOmniboxState.force)) {
      publishSearchResult({
        eventIds: new Float64Array(0),
        tses: new BigInt64Array(0),
        utids: new Float64Array(0),
        sources: [],
        trackKeys: [],
        totalResults: 0,
      });
      return;
    }

    const computeResults = this.specificSearch(search).then((searchResults) => {
      publishSearchResult(searchResults);
    });

    Promise.all([computeResults]).finally(() => {
      this.updateInProgress = false;
      this.run();
    });
  }

  onDestroy() {}

  private async specificSearch(search: string) {
    const searchLiteral = escapeSearchQuery(search);
    // TODO(hjd): we should avoid recomputing this every time. This will be
    // easier once the track table has entries for all the tracks.
    const cpuToTrackId = new Map();
    for (const track of Object.values(globals.state.tracks)) {
      const trackInfo = globals.trackManager.resolveTrackInfo(track.uri);
      if (trackInfo?.tags?.kind === CPU_SLICE_TRACK_KIND) {
        exists(trackInfo.tags.cpu) &&
          cpuToTrackId.set(trackInfo.tags.cpu, track.key);
      }
    }

    const utidRes = await this.query(`select utid from thread join process
    using(upid) where
      thread.name glob ${searchLiteral} or
      process.name glob ${searchLiteral}`);
    const utids = [];
    for (const it = utidRes.iter({utid: NUM}); it.valid(); it.next()) {
      utids.push(it.utid);
    }

    const res = await this.query(`
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
          where slice.name glob ${searchLiteral}
            or (
              0 != CAST(${sqliteString(search)} AS INT) and
              sliceId = CAST(${sqliteString(search)} AS INT)
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
        where string_value glob ${searchLiteral} or key glob ${searchLiteral}
      )
      union all
      select
        id as sliceId,
        ts,
        'log' as source,
        0 as sourceId,
        utid
      from android_logs where msg glob ${searchLiteral}
      order by ts
    `);

    const searchResults: CurrentSearchResults = {
      eventIds: new Float64Array(0),
      tses: new BigInt64Array(0),
      utids: new Float64Array(0),
      sources: [],
      trackKeys: [],
      totalResults: 0,
    };

    const lowerSearch = search.toLowerCase();
    for (const track of Object.values(globals.state.tracks)) {
      if (track.name.toLowerCase().indexOf(lowerSearch) === -1) {
        continue;
      }
      searchResults.totalResults++;
      searchResults.sources.push('track');
      searchResults.trackKeys.push(track.key);
    }

    const rows = res.numRows();
    searchResults.eventIds = new Float64Array(
      searchResults.totalResults + rows,
    );
    searchResults.tses = new BigInt64Array(searchResults.totalResults + rows);
    searchResults.utids = new Float64Array(searchResults.totalResults + rows);
    for (let i = 0; i < searchResults.totalResults; ++i) {
      searchResults.eventIds[i] = -1;
      searchResults.tses[i] = -1n;
      searchResults.utids[i] = -1;
    }

    const it = res.iter({
      sliceId: NUM,
      ts: LONG,
      source: STR,
      sourceId: NUM,
      utid: NUM,
    });
    for (; it.valid(); it.next()) {
      let trackId = undefined;
      if (it.source === 'cpu') {
        trackId = cpuToTrackId.get(it.sourceId);
      } else if (it.source === 'slice') {
        trackId = globals.trackManager.trackKeyByTrackId.get(it.sourceId);
      } else if (it.source === 'log') {
        const logTracks = Object.values(globals.state.tracks).filter(
          (track) => {
            const trackDesc = globals.trackManager.resolveTrackInfo(track.uri);
            return trackDesc && trackDesc.tags?.kind === 'AndroidLogTrack';
          },
        );
        if (logTracks.length > 0) {
          trackId = logTracks[0].key;
        }
      }

      // The .get() calls above could return undefined, this isn't just an else.
      if (trackId === undefined) {
        continue;
      }

      const i = searchResults.totalResults++;
      searchResults.trackKeys.push(trackId);
      searchResults.sources.push(it.source as SearchSource);
      searchResults.eventIds[i] = it.sliceId;
      searchResults.tses[i] = it.ts;
      searchResults.utids[i] = it.utid;
    }
    return searchResults;
  }

  private async query(query: string) {
    const result = await this.engine.query(query);
    return result;
  }
}
