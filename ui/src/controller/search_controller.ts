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
import {Engine} from '../common/engine';
import {NUM, STR} from '../common/query_result';
import {escapeSearchQuery} from '../common/query_utils';
import {CurrentSearchResults, SearchSummary} from '../common/search_data';
import {TimeSpan} from '../common/time';
import {publishSearch, publishSearchResult} from '../frontend/publish';

import {Controller} from './controller';
import {App} from './globals';
import {toNs} from '../common/time';

export interface SearchControllerArgs {
  engine: Engine;
  app: App;
}

export class SearchController extends Controller<'main'> {
  private engine: Engine;
  private app: App;
  private previousSpan: TimeSpan;
  private previousResolution: number;
  private previousSearch: string;
  private updateInProgress: boolean;
  private setupInProgress: boolean;

  constructor(args: SearchControllerArgs) {
    super('main');
    this.engine = args.engine;
    this.app = args.app;
    this.previousSpan = new TimeSpan(0, 1);
    this.previousSearch = '';
    this.updateInProgress = false;
    this.setupInProgress = true;
    this.previousResolution = 1;
    this.setup().finally(() => {
      this.setupInProgress = false;
      this.run();
    });
  }

  private async setup() {
    await this.query(`create virtual table search_summary_window
      using window;`);
    await this.query(`create virtual table search_summary_sched_span using
      span_join(sched PARTITIONED cpu, search_summary_window);`);
    await this.query(`create virtual table search_summary_slice_span using
      span_join(slice PARTITIONED track_id, search_summary_window);`);
  }

  run() {
    if (this.setupInProgress || this.updateInProgress) {
      return;
    }

    const visibleState = this.app.state.frontendLocalState.visibleState;
    const omniboxState = this.app.state.omniboxState;
    if (visibleState === undefined || omniboxState === undefined ||
        omniboxState.mode === 'COMMAND') {
      return;
    }
    const newSpan = new TimeSpan(visibleState.startSec, visibleState.endSec);
    const newSearch = omniboxState.omnibox;
    let newResolution = visibleState.resolution;
    if (this.previousSpan.contains(newSpan) &&
        this.previousResolution === newResolution &&
        newSearch === this.previousSearch) {
      return;
    }


    // TODO(hjd): We should restrict this to the start of the trace but
    // that is not easily available here.
    // N.B. Timestamps can be negative.
    const start = newSpan.start - newSpan.duration;
    const end = newSpan.end + newSpan.duration;
    this.previousSpan = new TimeSpan(start, end);
    this.previousResolution = newResolution;
    this.previousSearch = newSearch;
    if (newSearch === '' || newSearch.length < 4) {
      publishSearch({
        tsStarts: new Float64Array(0),
        tsEnds: new Float64Array(0),
        count: new Uint8Array(0),
      });
      publishSearchResult({
        sliceIds: new Float64Array(0),
        tsStarts: new Float64Array(0),
        utids: new Float64Array(0),
        sources: [],
        trackIds: [],
        totalResults: 0,
      });
      return;
    }

    let startNs = toNs(newSpan.start);
    let endNs = toNs(newSpan.end);

    // TODO(hjd): We shouldn't need to be so defensive here:
    if (!Number.isFinite(startNs)) {
      startNs = 0;
    }
    if (!Number.isFinite(endNs)) {
      endNs = 1;
    }
    if (!Number.isFinite(newResolution)) {
      newResolution = 1;
    }

    this.updateInProgress = true;
    const computeSummary = this.update(newSearch, startNs, endNs, newResolution)
                               .then((summary) => {
                                 publishSearch(summary);
                               });

    const computeResults =
        this.specificSearch(newSearch).then((searchResults) => {
          publishSearchResult(searchResults);
        });

    Promise.all([computeSummary, computeResults])
        .finally(() => {
          this.updateInProgress = false;
          this.run();
        });
  }

  onDestroy() {}

  private async update(
      search: string, startNs: number, endNs: number,
      resolution: number): Promise<SearchSummary> {
    const quantumNs = Math.round(resolution * 10 * 1e9);

    const searchLiteral = escapeSearchQuery(search);

    startNs = Math.floor(startNs / quantumNs) * quantumNs;

    const windowDur = Math.max(endNs - startNs, 1);
    await this.query(`update search_summary_window set
      window_start=${startNs},
      window_dur=${windowDur},
      quantum=${quantumNs}
      where rowid = 0;`);

    const utidRes = await this.query(`select utid from thread join process
      using(upid) where thread.name glob ${searchLiteral}
      or process.name glob ${searchLiteral}`);

    const utids = [];
    for (const it = utidRes.iter({utid: NUM}); it.valid(); it.next()) {
      utids.push(it.utid);
    }

    const cpus = await this.engine.getCpus();
    const maxCpu = Math.max(...cpus, -1);

    const res = await this.query(`
        select
          (quantum_ts * ${quantumNs} + ${startNs})/1e9 as tsStart,
          ((quantum_ts+1) * ${quantumNs} + ${startNs})/1e9 as tsEnd,
          min(count(*), 255) as count
          from (
              select
              quantum_ts
              from search_summary_sched_span
              where utid in (${utids.join(',')}) and cpu <= ${maxCpu}
            union all
              select
              quantum_ts
              from search_summary_slice_span
              where name glob ${searchLiteral}
          )
          group by quantum_ts
          order by quantum_ts;`);

    const numRows = res.numRows();
    const summary = {
      tsStarts: new Float64Array(numRows),
      tsEnds: new Float64Array(numRows),
      count: new Uint8Array(numRows),
    };

    const it = res.iter({tsStart: NUM, tsEnd: NUM, count: NUM});
    for (let row = 0; it.valid(); it.next(), ++row) {
      summary.tsStarts[row] = it.tsStart;
      summary.tsEnds[row] = it.tsEnd;
      summary.count[row] = it.count;
    }
    return summary;
  }

  private async specificSearch(search: string) {
    const searchLiteral = escapeSearchQuery(search);
    // TODO(hjd): we should avoid recomputing this every time. This will be
    // easier once the track table has entries for all the tracks.
    const cpuToTrackId = new Map();
    for (const track of Object.values(this.app.state.tracks)) {
      if (track.kind === 'CpuSliceTrack') {
        cpuToTrackId.set((track.config as {cpu: number}).cpu, track.id);
        continue;
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

    const queryRes = await this.query(`
    select
      id as sliceId,
      ts,
      'cpu' as source,
      cpu as sourceId,
      utid
    from sched where utid in (${utids.join(',')})
    union
    select
      slice_id as sliceId,
      ts,
      'track' as source,
      track_id as sourceId,
      0 as utid
      from slice
      where slice.name glob ${searchLiteral}
        or (
          0 != CAST(${(sqliteString(search))} AS INT) and
          sliceId = CAST(${(sqliteString(search))} AS INT)
        )
    union
    select
      slice_id as sliceId,
      ts,
      'track' as source,
      track_id as sourceId,
      0 as utid
      from slice
      join args using(arg_set_id)
      where string_value glob ${searchLiteral} or key glob ${searchLiteral}
    union
    select
      id as sliceId,
      ts,
      'log' as source,
      0 as sourceId,
      utid
    from android_logs where msg glob ${searchLiteral}
    order by ts

    `);

    const rows = queryRes.numRows();
    const searchResults: CurrentSearchResults = {
      sliceIds: new Float64Array(rows),
      tsStarts: new Float64Array(rows),
      utids: new Float64Array(rows),
      trackIds: [],
      sources: [],
      totalResults: 0,
    };

    const it = queryRes.iter(
        {sliceId: NUM, ts: NUM, source: STR, sourceId: NUM, utid: NUM});
    for (; it.valid(); it.next()) {
      let trackId = undefined;
      if (it.source === 'cpu') {
        trackId = cpuToTrackId.get(it.sourceId);
      } else if (it.source === 'track') {
        trackId = this.app.state.uiTrackIdByTraceTrackId[it.sourceId];
      } else if (it.source === 'log') {
        const logTracks = Object.values(this.app.state.tracks)
                              .filter((t) => t.kind === 'AndroidLogTrack');
        if (logTracks.length > 0) {
          trackId = logTracks[0].id;
        }
      }

      // The .get() calls above could return undefined, this isn't just an else.
      if (trackId === undefined) {
        continue;
      }

      const i = searchResults.totalResults++;
      searchResults.trackIds.push(trackId);
      searchResults.sources.push(it.source);
      searchResults.sliceIds[i] = it.sliceId;
      searchResults.tsStarts[i] = it.ts;
      searchResults.utids[i] = it.utid;
    }
    return searchResults;
  }

  private async query(query: string) {
    const result = await this.engine.query(query);
    return result;
  }
}
