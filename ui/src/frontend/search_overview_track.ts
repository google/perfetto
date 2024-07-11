// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {Duration, Time, TimeSpan, duration, time} from '../base/time';
import {Size} from '../base/geom';
import {PxSpan, TimeScale} from './time_scale';
import {AsyncLimiter} from '../base/async_limiter';
import {AsyncDisposableStack} from '../base/disposable_stack';
import {createVirtualTable} from '../trace_processor/sql_utils';
import {SearchSummary} from '../common/search_data';
import {escapeSearchQuery} from '../trace_processor/query_utils';
import {calculateResolution} from '../common/resolution';
import {OmniboxState} from '../common/state';
import {Optional} from '../base/utils';
import {AppContext} from './app_context';
import {Engine} from '../trace_processor/engine';
import {LONG, NUM} from '../trace_processor/query_result';

export interface SearchOverviewTrack extends AsyncDisposable {
  render(ctx: CanvasRenderingContext2D, size: Size): void;
}

/**
 * This function describes a pseudo-track that renders the search overview
 * blobs.
 *
 * @param engine The engine to use for loading data.
 * @returns A new search overview renderer.
 */
export async function createSearchOverviewTrack(
  engine: Engine,
  app: AppContext,
): Promise<SearchOverviewTrack> {
  const trash = new AsyncDisposableStack();
  trash.use(
    await createVirtualTable(engine, 'search_summary_window', 'window'),
  );
  trash.use(
    await createVirtualTable(
      engine,
      'search_summary_sched_span',
      'span_join(sched PARTITIONED cpu, search_summary_window)',
    ),
  );
  trash.use(
    await createVirtualTable(
      engine,
      'search_summary_slice_span',
      'span_join(slice PARTITIONED track_id, search_summary_window)',
    ),
  );

  let previousResolution: duration;
  let previousSpan: TimeSpan;
  let previousOmniboxState: OmniboxState;
  let searchSummary: Optional<SearchSummary>;
  const limiter = new AsyncLimiter();

  async function update(
    search: string,
    start: time,
    end: time,
    resolution: duration,
  ): Promise<SearchSummary> {
    const searchLiteral = escapeSearchQuery(search);

    const resolutionScalingFactor = 10n;
    const quantum = resolution * resolutionScalingFactor;
    start = Time.quantFloor(start, quantum);

    const windowDur = Duration.max(Time.diff(end, start), 1n);
    await engine.query(`update search_summary_window set
      window_start=${start},
      window_dur=${windowDur},
      quantum=${quantum}
      where rowid = 0;`);

    const utidRes = await engine.query(`select utid from thread join process
      using(upid) where thread.name glob ${searchLiteral}
      or process.name glob ${searchLiteral}`);

    const utids = [];
    for (const it = utidRes.iter({utid: NUM}); it.valid(); it.next()) {
      utids.push(it.utid);
    }

    const cpus = app.traceContext.cpus;
    const maxCpu = Math.max(...cpus, -1);

    const res = await engine.query(`
        select
          (quantum_ts * ${quantum} + ${start}) as tsStart,
          ((quantum_ts+1) * ${quantum} + ${start}) as tsEnd,
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
    const summary: SearchSummary = {
      tsStarts: new BigInt64Array(numRows),
      tsEnds: new BigInt64Array(numRows),
      count: new Uint8Array(numRows),
    };

    const it = res.iter({tsStart: LONG, tsEnd: LONG, count: NUM});
    for (let row = 0; it.valid(); it.next(), ++row) {
      summary.tsStarts[row] = it.tsStart;
      summary.tsEnds[row] = it.tsEnd;
      summary.count[row] = it.count;
    }
    return summary;
  }

  function maybeUpdate(size: Size) {
    const omniboxState = app.state.omniboxState;
    if (omniboxState === undefined || omniboxState.mode === 'COMMAND') {
      return;
    }
    const newSpan = app.timeline.visibleWindow;
    const newOmniboxState = omniboxState;
    const newResolution = calculateResolution(newSpan, size.width);
    const newTimeSpan = newSpan.toTimeSpan();
    if (
      previousSpan?.containsSpan(newTimeSpan.start, newTimeSpan.end) &&
      previousResolution === newResolution &&
      previousOmniboxState === newOmniboxState
    ) {
      return;
    }

    // TODO(hjd): We should restrict this to the start of the trace but
    // that is not easily available here.
    // N.B. Timestamps can be negative.
    const {start, end} = newTimeSpan.pad(newTimeSpan.duration);
    previousSpan = new TimeSpan(start, end);
    previousResolution = newResolution;
    previousOmniboxState = newOmniboxState;
    const search = newOmniboxState.omnibox;
    if (search === '' || (search.length < 4 && !newOmniboxState.force)) {
      searchSummary = {
        tsStarts: new BigInt64Array(0),
        tsEnds: new BigInt64Array(0),
        count: new Uint8Array(0),
      };
      return;
    }

    limiter.schedule(async () => {
      const summary = await update(
        newOmniboxState.omnibox,
        start,
        end,
        newResolution,
      );
      searchSummary = summary;
    });
  }

  function renderSearchOverview(
    ctx: CanvasRenderingContext2D,
    size: Size,
  ): void {
    const visibleWindow = app.timeline.visibleWindow;
    const timescale = new TimeScale(visibleWindow, new PxSpan(0, size.width));

    if (!searchSummary) return;

    for (let i = 0; i < searchSummary.tsStarts.length; i++) {
      const tStart = Time.fromRaw(searchSummary.tsStarts[i]);
      const tEnd = Time.fromRaw(searchSummary.tsEnds[i]);
      if (!visibleWindow.overlaps(tStart, tEnd)) {
        continue;
      }
      const rectStart = Math.max(timescale.timeToPx(tStart), 0);
      const rectEnd = timescale.timeToPx(tEnd);
      ctx.fillStyle = '#ffe263';
      ctx.fillRect(
        Math.floor(rectStart),
        0,
        Math.ceil(rectEnd - rectStart),
        size.height,
      );
    }
    const index = app.state.searchIndex;
    if (index !== -1 && index < app.currentSearchResults.tses.length) {
      const start = app.currentSearchResults.tses[index];
      if (start !== -1n) {
        const triangleStart = Math.max(
          timescale.timeToPx(Time.fromRaw(start)),
          0,
        );
        ctx.fillStyle = '#000';
        ctx.beginPath();
        ctx.moveTo(triangleStart, size.height);
        ctx.lineTo(triangleStart - 3, 0);
        ctx.lineTo(triangleStart + 3, 0);
        ctx.lineTo(triangleStart, size.height);
        ctx.fill();
        ctx.closePath();
      }
    }

    ctx.restore();
  }

  return {
    render(ctx: CanvasRenderingContext2D, size: Size) {
      maybeUpdate(size);
      renderSearchOverview(ctx, size);
    },
    async [Symbol.asyncDispose](): Promise<void> {
      return await trash.asyncDispose();
    },
  };
}
