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

import {Vnode} from 'mithril';

import {colorForString} from '../../common/colorizer';
import {PluginContext} from '../../common/plugin_api';
import {NUM, STR} from '../../common/query_result';
import {fromNs, toNsCeil, toNsFloor} from '../../common/time';
import {TrackData} from '../../common/track_data';
import {LIMIT} from '../../common/track_data';
import {
  TrackController,
} from '../../controller/track_controller';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {NewTrackArgs, Track} from '../../frontend/track';


export interface Data extends TrackData {
  // Total number of  events within [start, end], before any quantization.
  numEvents: number;

  // Below: data quantized by resolution and aggregated by event priority.
  timestamps: Float64Array;

  names: string[];
}

export interface Config {
  cpu?: number;
}

export const FTRACE_RAW_TRACK_KIND = 'FtraceRawTrack';

const MARGIN = 2;
const RECT_HEIGHT = 18;
const TRACK_HEIGHT = (RECT_HEIGHT) + (2 * MARGIN);

class FtraceRawTrackController extends TrackController<Config, Data> {
  static readonly kind = FTRACE_RAW_TRACK_KIND;

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const startNs = toNsFloor(start);
    const endNs = toNsCeil(end);

    // |resolution| is in s/px the frontend wants.
    const quantNs = toNsCeil(resolution);

    const excludeList = Array.from(globals.state.ftraceFilter.excludedNames);
    const excludeListSql = excludeList.map((s) => `'${s}'`).join(',');
    const cpuFilter =
        this.config.cpu === undefined ? '' : `and cpu = ${this.config.cpu}`;

    const queryRes = await this.query(`
      select
        cast(ts / ${quantNs} as integer) * ${quantNs} as tsQuant,
        type,
        count(type) as numEvents,
        name
      from raw
      where name not in (${excludeListSql}) and ts >= ${startNs} and ts <= ${
        endNs} ${cpuFilter}
      group by tsQuant
      order by tsQuant limit ${LIMIT};`);

    const rowCount = queryRes.numRows();
    const result = {
      start,
      end,
      resolution,
      length: rowCount,
      numEvents: 0,
      timestamps: new Float64Array(rowCount),
      names: [],
    } as Data;

    const it = queryRes.iter(
        {tsQuant: NUM, type: STR, numEvents: NUM, name: STR},
    );
    for (let row = 0; it.valid(); it.next(), row++) {
      result.timestamps[row] = fromNs(it.tsQuant);
      result.names[row] = it.name;
      result.numEvents += it.numEvents;
    }
    return result;
  }
}

export class FtraceRawTrack extends Track<Config, Data> {
  static readonly kind = FTRACE_RAW_TRACK_KIND;
  constructor(args: NewTrackArgs) {
    super(args);
  }

  static create(args: NewTrackArgs): FtraceRawTrack {
    return new FtraceRawTrack(args);
  }

  getHeight(): number {
    return TRACK_HEIGHT;
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

    const diamondSideLen = RECT_HEIGHT / Math.sqrt(2);

    for (let i = 0; i < data.timestamps.length; i++) {
      const name = data.names[i];
      const color = colorForString(name);
      const hsl = `hsl(
        ${color.h},
        ${color.s - 20}%,
        ${Math.min(color.l + 10, 60)}%
      )`;
      ctx.fillStyle = hsl;
      const xPos = Math.floor(timeScale.timeToPx(data.timestamps[i]));

      // Draw a diamond over the event
      ctx.save();
      ctx.translate(xPos, MARGIN);
      ctx.rotate(Math.PI / 4);
      ctx.fillRect(0, 0, diamondSideLen, diamondSideLen);
      ctx.restore();
    }
  }

  getContextMenu(): Vnode<any, {}>|null {
    return null;
  }
}

function activate(ctx: PluginContext) {
  ctx.registerTrack(FtraceRawTrack);
  ctx.registerTrackController(FtraceRawTrackController);
}

export const plugin = {
  pluginId: 'perfetto.FtraceRaw',
  activate,
};
