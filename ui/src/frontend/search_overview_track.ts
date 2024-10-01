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

import {AsyncLimiter} from '../base/async_limiter';
import {AsyncDisposableStack} from '../base/disposable_stack';
import {Size2D} from '../base/geom';
import {Duration, Time, TimeSpan, duration, time} from '../base/time';
import {TimeScale} from '../base/time_scale';
import {Optional} from '../base/utils';
import {calculateResolution} from '../common/resolution';
import {TraceImpl} from '../core/trace_impl';
import {LONG, NUM} from '../trace_processor/query_result';
import {escapeSearchQuery} from '../trace_processor/query_utils';
import {createVirtualTable} from '../trace_processor/sql_utils';

export interface SearchOverviewTrack extends AsyncDisposable {
  render(ctx: CanvasRenderingContext2D, size: Size2D): void;
}

interface SearchSummary {
  tsStarts: BigInt64Array;
  tsEnds: BigInt64Array;
  count: Uint8Array;
}

/**
 * This function describes a pseudo-track that renders the search overview
 * blobs.
 *
 * @returns A new search overview renderer.
 */
export async function createSearchOverviewTrack(
  trace: TraceImpl,
): Promise<SearchOverviewTrack> {
  const trash = new AsyncDisposableStack();
  const engine = trace.engine;
  const searchManager = trace.search;
  const timeline = trace.timeline;
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
  let previousSearchGeneration = 0;
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

    const res = await engine.query(`
        select
          (quantum_ts * ${quantum} + ${start}) as tsStart,
          ((quantum_ts+1) * ${quantum} + ${start}) as tsEnd,
          min(count(*), 255) as count
          from (
              select
              quantum_ts
              from search_summary_sched_span
              where utid in (${utids.join(',')})
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

  function maybeUpdate(size: Size2D) {
    if (!searchManager.hasResults) {
      return;
    }
    const newSpan = timeline.visibleWindow;
    const newSearchGeneration = searchManager.searchGeneration;
    const newResolution = calculateResolution(newSpan, size.width);
    const newTimeSpan = newSpan.toTimeSpan();
    if (
      previousSpan?.containsSpan(newTimeSpan.start, newTimeSpan.end) &&
      previousResolution === newResolution &&
      previousSearchGeneration === newSearchGeneration
    ) {
      return;
    }

    // TODO(hjd): We should restrict this to the start of the trace but
    // that is not easily available here.
    // N.B. Timestamps can be negative.
    const {start, end} = newTimeSpan.pad(newTimeSpan.duration);
    previousSpan = new TimeSpan(start, end);
    previousResolution = newResolution;
    previousSearchGeneration = newSearchGeneration;
    const search = searchManager.searchText;
    if (search === '') {
      searchSummary = {
        tsStarts: new BigInt64Array(0),
        tsEnds: new BigInt64Array(0),
        count: new Uint8Array(0),
      };
      return;
    }

    limiter.schedule(async () => {
      const summary = await update(
        searchManager.searchText,
        start,
        end,
        newResolution,
      );
      searchSummary = summary;
    });
  }

  function renderSearchOverview(
    ctx: CanvasRenderingContext2D,
    size: Size2D,
  ): void {
    const visibleWindow = timeline.visibleWindow;
    const timescale = new TimeScale(visibleWindow, {
      left: 0,
      right: size.width,
    });

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
    const results = searchManager.searchResults;
    if (results === undefined) {
      return;
    }
    const index = searchManager.resultIndex;
    if (index !== -1 && index < results.tses.length) {
      const start = results.tses[index];
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
    render(ctx: CanvasRenderingContext2D, size: Size2D) {
      maybeUpdate(size);
      renderSearchOverview(ctx, size);
    },
    async [Symbol.asyncDispose](): Promise<void> {
      return await trash.asyncDispose();
    },
  };
}
