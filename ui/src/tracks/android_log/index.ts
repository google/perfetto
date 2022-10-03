// Copyright (C) 2021 The Android Open Source Project
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

import {PluginContext} from '../../common/plugin_api';
import {NUM} from '../../common/query_result';
import {fromNs, toNsCeil, toNsFloor} from '../../common/time';
import {TrackData} from '../../common/track_data';
import {LIMIT} from '../../common/track_data';
import {
  TrackController,
} from '../../controller/track_controller';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {NewTrackArgs, Track} from '../../frontend/track';

export const ANDROID_LOGS_TRACK_KIND = 'AndroidLogTrack';

export interface Data extends TrackData {
  // Total number of log events within [start, end], before any quantization.
  numEvents: number;

  // Below: data quantized by resolution and aggregated by event priority.

  timestamps: Float64Array;

  // Each Uint8 value has the i-th bit is set if there is at least one log
  // event at the i-th priority level at the corresponding time in |timestamps|.
  priorities: Uint8Array;
}

export interface Config {}

interface LevelCfg {
  color: string;
  prios: number[];
}

const LEVELS: LevelCfg[] = [
  {color: 'hsl(122, 39%, 49%)', prios: [0, 1, 2, 3]},  // Up to DEBUG: Green.
  {color: 'hsl(0, 0%, 70%)', prios: [4]},              // 4 (INFO) -> Gray.
  {color: 'hsl(45, 100%, 51%)', prios: [5]},           // 5 (WARN) -> Amber.
  {color: 'hsl(4, 90%, 58%)', prios: [6]},             // 6 (ERROR) -> Red.
  {color: 'hsl(291, 64%, 42%)', prios: [7]},           // 7 (FATAL) -> Purple
];

const MARGIN_TOP = 2;
const RECT_HEIGHT = 35;
const EVT_PX = 2;  // Width of an event tick in pixels.

class AndroidLogTrackController extends TrackController<Config, Data> {
  static readonly kind = ANDROID_LOGS_TRACK_KIND;

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const startNs = toNsFloor(start);
    const endNs = toNsCeil(end);

    // |resolution| is in s/px the frontend wants.
    const quantNs = toNsCeil(resolution);

    const queryRes = await this.query(`
      select
        cast(ts / ${quantNs} as integer) * ${quantNs} as tsQuant,
        prio,
        count(prio) as numEvents
      from android_logs
      where ts >= ${startNs} and ts <= ${endNs}
      group by tsQuant, prio
      order by tsQuant, prio limit ${LIMIT};`);

    const rowCount = queryRes.numRows();
    const result = {
      start,
      end,
      resolution,
      length: rowCount,
      numEvents: 0,
      timestamps: new Float64Array(rowCount),
      priorities: new Uint8Array(rowCount),
    };


    const it = queryRes.iter({tsQuant: NUM, prio: NUM, numEvents: NUM});
    for (let row = 0; it.valid(); it.next(), row++) {
      result.timestamps[row] = fromNs(it.tsQuant);
      const prio = Math.min(it.prio, 7);
      result.priorities[row] |= (1 << prio);
      result.numEvents += it.numEvents;
    }
    return result;
  }
}

class AndroidLogTrack extends Track<Config, Data> {
  static readonly kind = ANDROID_LOGS_TRACK_KIND;
  static create(args: NewTrackArgs): AndroidLogTrack {
    return new AndroidLogTrack(args);
  }

  constructor(args: NewTrackArgs) {
    super(args);
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    const {timeScale, visibleWindowTime} = globals.frontendLocalState;

    const data = this.data();

    if (data === undefined) return;  // Can't possibly draw anything.

    const dataStartPx = timeScale.timeToPx(data.start);
    const dataEndPx = timeScale.timeToPx(data.end);
    const visibleStartPx = timeScale.timeToPx(visibleWindowTime.start);
    const visibleEndPx = timeScale.timeToPx(visibleWindowTime.end);

    checkerboardExcept(
        ctx,
        this.getHeight(),
        visibleStartPx,
        visibleEndPx,
        dataStartPx,
        dataEndPx);

    const quantWidth =
        Math.max(EVT_PX, timeScale.deltaTimeToPx(data.resolution));
    const blockH = RECT_HEIGHT / LEVELS.length;
    for (let i = 0; i < data.timestamps.length; i++) {
      for (let lev = 0; lev < LEVELS.length; lev++) {
        let hasEventsForCurColor = false;
        for (const prio of LEVELS[lev].prios) {
          if (data.priorities[i] & (1 << prio)) hasEventsForCurColor = true;
        }
        if (!hasEventsForCurColor) continue;
        ctx.fillStyle = LEVELS[lev].color;
        const px = Math.floor(timeScale.timeToPx(data.timestamps[i]));
        ctx.fillRect(px, MARGIN_TOP + blockH * lev, quantWidth, blockH);
      }  // for(lev)
    }    // for (timestamps)
  }
}

function activate(ctx: PluginContext) {
  ctx.registerTrack(AndroidLogTrack);
  ctx.registerTrackController(AndroidLogTrackController);
}

export const plugin = {
  pluginId: 'perfetto.AndroidLog',
  activate,
};
