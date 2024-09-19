// Copyright (C) 2023 The Android Open Source Project
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

import {BigintMath} from '../../base/bigint_math';
import {assertExists, assertTrue} from '../../base/logging';
import {duration, Time, time} from '../../base/time';
import {colorForTid} from '../../core/colorizer';
import {TrackData} from '../../common/track_data';
import {TimelineFetcher} from '../../common/track_helper';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {Engine} from '../../trace_processor/engine';
import {Track} from '../../public/track';
import {LONG, NUM} from '../../trace_processor/query_result';
import {uuidv4Sql} from '../../base/uuid';
import {TrackRenderContext} from '../../public/track';

export const PROCESS_SUMMARY_TRACK = 'ProcessSummaryTrack';

interface Data extends TrackData {
  starts: BigInt64Array;
  utilizations: Float64Array;
}

export interface Config {
  pidForColor: number;
  upid: number | null;
  utid: number | null;
}

const MARGIN_TOP = 5;
const RECT_HEIGHT = 30;
const TRACK_HEIGHT = MARGIN_TOP * 2 + RECT_HEIGHT;
const SUMMARY_HEIGHT = TRACK_HEIGHT - MARGIN_TOP;

export class ProcessSummaryTrack implements Track {
  private fetcher = new TimelineFetcher<Data>(this.onBoundsChange.bind(this));
  private engine: Engine;
  private config: Config;
  private uuid = uuidv4Sql();

  constructor(engine: Engine, config: Config) {
    this.engine = engine;
    this.config = config;
  }

  async onCreate(): Promise<void> {
    let trackIdQuery: string;
    if (this.config.upid !== null) {
      trackIdQuery = `
        select tt.id as track_id
        from thread_track as tt
        join _thread_available_info_summary using (utid)
        join thread using (utid)
        where thread.upid = ${this.config.upid}
        order by slice_count desc
      `;
    } else {
      trackIdQuery = `
        select tt.id as track_id
        from thread_track as tt
        join _thread_available_info_summary using (utid)
        where tt.utid = ${assertExists(this.config.utid)}
        order by slice_count desc
      `;
    }
    await this.engine.query(`
      create virtual table process_summary_${this.uuid}
      using __intrinsic_counter_mipmap((
        with
          tt as materialized (
            ${trackIdQuery}
          ),
          ss as (
            select ts, 1.0 as value
            from slice
            join tt using (track_id)
            where slice.depth = 0
            union all
            select ts + dur as ts, -1.0 as value
            from slice
            join tt using (track_id)
            where slice.depth = 0
          )
        select
          ts,
          sum(value) over (order by ts) / (select count() from tt) as value
        from ss
        order by ts
      ));
    `);
  }

  async onUpdate({
    visibleWindow,
    resolution,
  }: TrackRenderContext): Promise<void> {
    await this.fetcher.requestData(visibleWindow.toTimeSpan(), resolution);
  }

  async onBoundsChange(
    start: time,
    end: time,
    resolution: duration,
  ): Promise<Data> {
    // Resolution must always be a power of 2 for this logic to work
    assertTrue(
      BigintMath.popcount(resolution) === 1,
      `${resolution} not pow of 2`,
    );

    const queryRes = await this.engine.query(`
      select last_ts as ts, last_value as utilization
      from process_summary_${this.uuid}(${start}, ${end}, ${resolution});
    `);
    const numRows = queryRes.numRows();
    const slices: Data = {
      start,
      end,
      resolution,
      length: numRows,
      starts: new BigInt64Array(numRows),
      utilizations: new Float64Array(numRows),
    };
    const it = queryRes.iter({
      ts: LONG,
      utilization: NUM,
    });
    for (let row = 0; it.valid(); it.next(), row++) {
      slices.starts[row] = it.ts;
      slices.utilizations[row] = it.utilization;
    }
    return slices;
  }

  async onDestroy(): Promise<void> {
    await this.engine.tryQuery(
      `drop table if exists process_summary_${this.uuid};`,
    );
    this.fetcher[Symbol.dispose]();
  }

  getHeight(): number {
    return TRACK_HEIGHT;
  }

  render(trackCtx: TrackRenderContext): void {
    const {ctx, size, timescale} = trackCtx;

    const data = this.fetcher.data;
    if (data === undefined) {
      return;
    }

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
      ctx,
      this.getHeight(),
      0,
      size.width,
      timescale.timeToPx(data.start),
      timescale.timeToPx(data.end),
    );

    this.renderSummary(trackCtx, data);
  }

  private renderSummary(
    {ctx, timescale}: TrackRenderContext,
    data: Data,
  ): void {
    const startPx = 0;
    const bottomY = TRACK_HEIGHT;

    let lastX = startPx;
    let lastY = bottomY;

    const color = colorForTid(this.config.pidForColor);
    ctx.fillStyle = color.base.cssString;
    ctx.beginPath();
    ctx.moveTo(lastX, lastY);
    for (let i = 0; i < data.utilizations.length; i++) {
      const startTime = Time.fromRaw(data.starts[i]);
      const utilization = data.utilizations[i];
      lastX = Math.floor(timescale.timeToPx(startTime));
      ctx.lineTo(lastX, lastY);
      lastY = MARGIN_TOP + Math.round(SUMMARY_HEIGHT * (1 - utilization));
      ctx.lineTo(lastX, lastY);
    }
    ctx.lineTo(lastX, bottomY);
    ctx.closePath();
    ctx.fill();
  }
}
