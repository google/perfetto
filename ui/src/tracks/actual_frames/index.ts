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

import {BigintMath as BIMath} from '../../base/bigint_math';
import {duration, time} from '../../base/time';
import {LONG, LONG_NULL, NUM, STR} from '../../common/query_result';
import {TrackData} from '../../common/track_data';
import {TrackController} from '../../controller/track_controller';
import {NewTrackArgs, Track} from '../../frontend/track';
import {Plugin, PluginContext, PluginInfo} from '../../public';
import {ChromeSliceTrack} from '../chrome_slices';

export const ACTUAL_FRAMES_SLICE_TRACK_KIND = 'ActualFramesSliceTrack';

export interface Config {
  maxDepth: number;
  trackIds: number[];
}

export interface Data extends TrackData {
  // Slices are stored in a columnar fashion. All fields have the same length.
  strings: string[];
  sliceIds: Float64Array;
  starts: BigInt64Array;
  ends: BigInt64Array;
  depths: Uint16Array;
  titles: Uint16Array;   // Index in |strings|.
  colors?: Uint16Array;  // Index in |strings|.
  isInstant: Uint16Array;
  isIncomplete: Uint16Array;
}

const BLUE_COLOR = '#03A9F4';         // Blue 500
const GREEN_COLOR = '#4CAF50';        // Green 500
const YELLOW_COLOR = '#FFEB3B';       // Yellow 500
const RED_COLOR = '#FF5722';          // Red 500
const LIGHT_GREEN_COLOR = '#C0D588';  // Light Green 500
const PINK_COLOR = '#F515E0';         // Pink 500

class ActualFramesSliceTrackController extends TrackController<Config, Data> {
  static readonly kind = ACTUAL_FRAMES_SLICE_TRACK_KIND;
  private maxDur = 0n;

  async onBoundsChange(start: time, end: time, resolution: duration):
      Promise<Data> {
    if (this.maxDur === 0n) {
      const maxDurResult = await this.query(`
        select
          max(iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts, dur))
            as maxDur
        from experimental_slice_layout
        where filter_track_ids = '${this.config.trackIds.join(',')}'
      `);
      this.maxDur = maxDurResult.firstRow({maxDur: LONG_NULL}).maxDur || 0n;
    }

    const rawResult = await this.query(`
      SELECT
        (s.ts + ${resolution / 2n}) / ${resolution} * ${resolution} as tsq,
        s.ts as ts,
        max(iif(s.dur = -1, (SELECT end_ts FROM trace_bounds) - s.ts, s.dur))
            as dur,
        s.layout_depth as layoutDepth,
        s.name as name,
        s.id as id,
        s.dur = 0 as isInstant,
        s.dur = -1 as isIncomplete,
        CASE afs.jank_tag
          WHEN 'Self Jank' THEN '${RED_COLOR}'
          WHEN 'Other Jank' THEN '${YELLOW_COLOR}'
          WHEN 'Dropped Frame' THEN '${BLUE_COLOR}'
          WHEN 'Buffer Stuffing' THEN '${LIGHT_GREEN_COLOR}'
          WHEN 'SurfaceFlinger Stuffing' THEN '${LIGHT_GREEN_COLOR}'
          WHEN 'No Jank' THEN '${GREEN_COLOR}'
          ELSE '${PINK_COLOR}'
        END as color
      from experimental_slice_layout s
      join actual_frame_timeline_slice afs using(id)
      where
        filter_track_ids = '${this.config.trackIds.join(',')}' and
        s.ts >= ${start - this.maxDur} and
        s.ts <= ${end}
      group by tsq, s.layout_depth
      order by tsq, s.layout_depth
    `);

    const numRows = rawResult.numRows();
    const slices: Data = {
      start,
      end,
      resolution,
      length: numRows,
      strings: [],
      sliceIds: new Float64Array(numRows),
      starts: new BigInt64Array(numRows),
      ends: new BigInt64Array(numRows),
      depths: new Uint16Array(numRows),
      titles: new Uint16Array(numRows),
      colors: new Uint16Array(numRows),
      isInstant: new Uint16Array(numRows),
      isIncomplete: new Uint16Array(numRows),
    };

    const stringIndexes = new Map<string, number>();
    function internString(str: string) {
      let idx = stringIndexes.get(str);
      if (idx !== undefined) return idx;
      idx = slices.strings.length;
      slices.strings.push(str);
      stringIndexes.set(str, idx);
      return idx;
    }

    const it = rawResult.iter({
      'tsq': LONG,
      'ts': LONG,
      'dur': LONG,
      'layoutDepth': NUM,
      'id': NUM,
      'name': STR,
      'isInstant': NUM,
      'isIncomplete': NUM,
      'color': STR,
    });
    for (let i = 0; it.valid(); i++, it.next()) {
      const startQ = it.tsq;
      const start = it.ts;
      const dur = it.dur;
      const end = start + dur;
      const minEnd = startQ + resolution;
      const endQ = BIMath.max(BIMath.quant(end, resolution), minEnd);

      slices.starts[i] = startQ;
      slices.ends[i] = endQ;
      slices.depths[i] = it.layoutDepth;
      slices.titles[i] = internString(it.name);
      slices.colors![i] = internString(it.color);
      slices.sliceIds[i] = it.id;
      slices.isInstant[i] = it.isInstant;
      slices.isIncomplete[i] = it.isIncomplete;
    }
    return slices;
  }
}

export class ActualFramesSliceTrack extends ChromeSliceTrack {
  static readonly kind = ACTUAL_FRAMES_SLICE_TRACK_KIND;
  static create(args: NewTrackArgs): Track {
    return new ActualFramesSliceTrack(args);
  }
}

class ActualFrames implements Plugin {
  onActivate(ctx: PluginContext): void {
    ctx.registerTrackController(ActualFramesSliceTrackController);
    ctx.registerTrack(ActualFramesSliceTrack);
  }
}

export const plugin: PluginInfo = {
  pluginId: 'perfetto.ActualFrames',
  plugin: ActualFrames,
};
