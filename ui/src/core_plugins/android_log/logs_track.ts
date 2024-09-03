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

import {Time, duration, time} from '../../base/time';
import {LIMIT, TrackData} from '../../common/track_data';
import {TimelineFetcher} from '../../common/track_helper';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM} from '../../trace_processor/query_result';
import {Track} from '../../public/track';
import {TrackRenderContext} from '../../public/track';

export interface Data extends TrackData {
  // Total number of log events within [start, end], before any quantization.
  numEvents: number;

  // Below: data quantized by resolution and aggregated by event priority.
  timestamps: BigInt64Array;

  // Each Uint8 value has the i-th bit is set if there is at least one log
  // event at the i-th priority level at the corresponding time in |timestamps|.
  priorities: Uint8Array;
}

const LEVELS: LevelCfg[] = [
  {color: 'hsl(122, 39%, 49%)', prios: [0, 1, 2, 3]}, // Up to DEBUG: Green.
  {color: 'hsl(0, 0%, 70%)', prios: [4]}, // 4 (INFO) -> Gray.
  {color: 'hsl(45, 100%, 51%)', prios: [5]}, // 5 (WARN) -> Amber.
  {color: 'hsl(4, 90%, 58%)', prios: [6]}, // 6 (ERROR) -> Red.
  {color: 'hsl(291, 64%, 42%)', prios: [7]}, // 7 (FATAL) -> Purple
];

const MARGIN_TOP = 2;
const RECT_HEIGHT = 35;
const EVT_PX = 2; // Width of an event tick in pixels.

interface LevelCfg {
  color: string;
  prios: number[];
}

export class AndroidLogTrack implements Track {
  private fetcher = new TimelineFetcher<Data>(this.onBoundsChange.bind(this));

  constructor(private engine: Engine) {}

  async onUpdate({
    visibleWindow,
    resolution,
  }: TrackRenderContext): Promise<void> {
    await this.fetcher.requestData(visibleWindow.toTimeSpan(), resolution);
  }

  async onDestroy(): Promise<void> {
    this.fetcher[Symbol.dispose]();
  }

  getHeight(): number {
    return 40;
  }

  async onBoundsChange(
    start: time,
    end: time,
    resolution: duration,
  ): Promise<Data> {
    const queryRes = await this.engine.query(`
      select
        cast(ts / ${resolution} as integer) * ${resolution} as tsQuant,
        prio,
        count(prio) as numEvents
      from android_logs
      where ts >= ${start} and ts <= ${end}
      group by tsQuant, prio
      order by tsQuant, prio limit ${LIMIT};`);

    const rowCount = queryRes.numRows();
    const result = {
      start,
      end,
      resolution,
      length: rowCount,
      numEvents: 0,
      timestamps: new BigInt64Array(rowCount),
      priorities: new Uint8Array(rowCount),
    };

    const it = queryRes.iter({tsQuant: LONG, prio: NUM, numEvents: NUM});
    for (let row = 0; it.valid(); it.next(), row++) {
      result.timestamps[row] = it.tsQuant;
      const prio = Math.min(it.prio, 7);
      result.priorities[row] |= 1 << prio;
      result.numEvents += it.numEvents;
    }
    return result;
  }

  render({ctx, size, timescale}: TrackRenderContext): void {
    const data = this.fetcher.data;

    if (data === undefined) return; // Can't possibly draw anything.

    const dataStartPx = timescale.timeToPx(data.start);
    const dataEndPx = timescale.timeToPx(data.end);

    checkerboardExcept(
      ctx,
      this.getHeight(),
      0,
      size.width,
      dataStartPx,
      dataEndPx,
    );

    const quantWidth = Math.max(
      EVT_PX,
      timescale.durationToPx(data.resolution),
    );
    const blockH = RECT_HEIGHT / LEVELS.length;
    for (let i = 0; i < data.timestamps.length; i++) {
      for (let lev = 0; lev < LEVELS.length; lev++) {
        let hasEventsForCurColor = false;
        for (const prio of LEVELS[lev].prios) {
          if (data.priorities[i] & (1 << prio)) hasEventsForCurColor = true;
        }
        if (!hasEventsForCurColor) continue;
        ctx.fillStyle = LEVELS[lev].color;
        const timestamp = Time.fromRaw(data.timestamps[i]);
        const px = Math.floor(timescale.timeToPx(timestamp));
        ctx.fillRect(px, MARGIN_TOP + blockH * lev, quantWidth, blockH);
      } // for(lev)
    } // for (timestamps)
  }
}
