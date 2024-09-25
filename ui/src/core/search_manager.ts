// Copyright (C) 2024 The Android Open Source Project
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

import {AsyncLimiter} from '../base/async_limiter';
import {searchSegment} from '../base/binary_search';
import {assertExists, assertTrue} from '../base/logging';
import {sqliteString} from '../base/string_utils';
import {Time} from '../base/time';
import {exists, Optional} from '../base/utils';
import {ResultStepEventHandler} from '../public/search';
import {
  ANDROID_LOGS_TRACK_KIND,
  CPU_SLICE_TRACK_KIND,
} from '../public/track_kinds';
import {Workspace} from '../public/workspace';
import {Engine} from '../trace_processor/engine';
import {LONG, NUM, STR} from '../trace_processor/query_result';
import {escapeSearchQuery} from '../trace_processor/query_utils';
import {raf} from './raf_scheduler';
import {SearchSource} from './search_data';
import {TimelineImpl} from './timeline';
import {TrackManagerImpl} from './track_manager';

export interface SearchResults {
  eventIds: Float64Array;
  tses: BigInt64Array;
  utids: Float64Array;
  trackUris: string[];
  sources: SearchSource[];
  totalResults: number;
}

export class SearchManagerImpl {
  private _searchGeneration = 0;
  private _searchText = '';
  private _results?: SearchResults;
  private _resultIndex = -1;
  private _searchInProgress = false;

  // TODO(primiano): once we get rid of globals, these below can be made always
  // defined. the ?: is to deal with globals-before-trace-load.
  private _timeline?: TimelineImpl;
  private _trackManager?: TrackManagerImpl;
  private _workspace?: Workspace;
  private _engine?: Engine;
  private _limiter = new AsyncLimiter();
  private _onResultStep?: ResultStepEventHandler;

  constructor(args?: {
    timeline: TimelineImpl;
    trackManager: TrackManagerImpl;
    workspace: Workspace;
    engine: Engine;
    onResultStep: ResultStepEventHandler;
  }) {
    this._timeline = args?.timeline;
    this._trackManager = args?.trackManager;
    this._engine = args?.engine;
    this._workspace = args?.workspace;
    this._onResultStep = args?.onResultStep;
  }

  search(text: string) {
    if (text === this._searchText) {
      return;
    }
    this._searchText = text;
    this._searchGeneration++;
    this._results = undefined;
    this._resultIndex = -1;
    this._searchInProgress = false;
    if (text !== '') {
      this._searchInProgress = true;
      this._limiter.schedule(async () => {
        await this.executeSearch();
        this._searchInProgress = false;
        raf.scheduleFullRedraw();
      });
    }
    raf.scheduleFullRedraw();
  }

  reset() {
    this.search('');
  }

  stepForward() {
    this.stepInternal(false);
  }

  stepBackwards() {
    this.stepInternal(true);
  }

  private stepInternal(reverse = false) {
    if (this._timeline === undefined) return;
    if (this._results === undefined) return;
    const index = this._resultIndex;
    const vizWindow = this._timeline.visibleWindow.toTimeSpan();
    const startNs = vizWindow.start;
    const endNs = vizWindow.end;
    const currentTs = this._results.tses[index];

    // If the value of |this._results.totalResults| is 0,
    // it means that the query is in progress or no results are found.
    if (this._results.totalResults === 0) {
      return;
    }

    // If this is a new search or the currentTs is not in the viewport,
    // select the first/last item in the viewport.
    if (
      index === -1 ||
      (currentTs !== -1n && (currentTs < startNs || currentTs > endNs))
    ) {
      if (reverse) {
        const [smaller] = searchSegment(this._results.tses, endNs);
        // If there is no item in the viewport just go to the previous.
        if (smaller === -1) {
          this.setResultIndexWithSaturation(index - 1);
        } else {
          this._resultIndex = smaller;
        }
      } else {
        const [, larger] = searchSegment(this._results.tses, startNs);
        // If there is no item in the viewport just go to the next.
        if (larger === -1) {
          this.setResultIndexWithSaturation(index + 1);
        } else {
          this._resultIndex = larger;
        }
      }
    } else {
      // If the currentTs is in the viewport, increment the index.
      if (reverse) {
        this.setResultIndexWithSaturation(index - 1);
      } else {
        this.setResultIndexWithSaturation(index + 1);
      }
    }
    if (this._onResultStep) {
      this._onResultStep({
        eventId: this._results.eventIds[this._resultIndex],
        ts: Time.fromRaw(this._results.tses[this._resultIndex]),
        trackUri: this._results.trackUris[this._resultIndex],
        source: this._results.sources[this._resultIndex],
      });
    }
    raf.scheduleFullRedraw();
  }

  private setResultIndexWithSaturation(nextIndex: number) {
    const tot = assertExists(this._results).totalResults;
    assertTrue(tot !== 0); // The early out in step() guarantees this.
    // This is a modulo operation that works also for negative numbers.
    this._resultIndex = ((nextIndex % tot) + tot) % tot;
  }

  get hasResults() {
    return this._results !== undefined;
  }

  get searchResults() {
    return this._results;
  }

  get resultIndex() {
    return this._resultIndex;
  }

  get searchText() {
    return this._searchText;
  }

  get searchGeneration() {
    return this._searchGeneration;
  }

  get searchInProgress(): boolean {
    return this._searchInProgress;
  }

  private async executeSearch() {
    const search = this._searchText;
    const searchLiteral = escapeSearchQuery(this._searchText);
    const generation = this._searchGeneration;

    const engine = this._engine;
    const trackManager = this._trackManager;
    const workspace = this._workspace;
    if (!engine || !trackManager || !workspace) {
      return;
    }

    // TODO(stevegolton): Avoid recomputing these indexes each time.
    const trackUrisByCpu = new Map<number, string>();
    const allTracks = trackManager.getAllTracks();
    allTracks.forEach((td) => {
      const tags = td?.tags;
      const cpu = tags?.cpu;
      const kind = tags?.kind;
      exists(cpu) &&
        kind === CPU_SLICE_TRACK_KIND &&
        trackUrisByCpu.set(cpu, td.uri);
    });

    const trackUrisByTrackId = new Map<number, string>();
    allTracks.forEach((td) => {
      const trackIds = td?.tags?.trackIds ?? [];
      trackIds.forEach((trackId) => trackUrisByTrackId.set(trackId, td.uri));
    });

    const utidRes = await engine.query(`select utid from thread join process
    using(upid) where
      thread.name glob ${searchLiteral} or
      process.name glob ${searchLiteral}`);
    const utids = [];
    for (const it = utidRes.iter({utid: NUM}); it.valid(); it.next()) {
      utids.push(it.utid);
    }

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

    const searchResults: SearchResults = {
      eventIds: new Float64Array(0),
      tses: new BigInt64Array(0),
      utids: new Float64Array(0),
      sources: [],
      trackUris: [],
      totalResults: 0,
    };

    const lowerSearch = search.toLowerCase();
    for (const track of workspace.flatTracks) {
      // We don't support searching for tracks that don't have a URI.
      if (!track.uri) continue;
      if (track.title.toLowerCase().indexOf(lowerSearch) === -1) {
        continue;
      }
      searchResults.totalResults++;
      searchResults.sources.push('track');
      searchResults.trackUris.push(track.uri);
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
      let track: Optional<string> = undefined;

      if (it.source === 'cpu') {
        track = trackUrisByCpu.get(it.sourceId);
      } else if (it.source === 'slice') {
        track = trackUrisByTrackId.get(it.sourceId);
      } else if (it.source === 'log') {
        track = trackManager
          .getAllTracks()
          .find((td) => td.tags?.kind === ANDROID_LOGS_TRACK_KIND)?.uri;
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

    if (generation !== this._searchGeneration) {
      // We arrived too late. By the time we computed results the user issued
      // another search.
      return;
    }
    this._results = searchResults;
    this._resultIndex = -1;
  }
}
