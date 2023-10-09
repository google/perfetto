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

import {TrackData} from '../../common/track_data';

export const EXPECTED_FRAMES_SLICE_TRACK_KIND = 'ExpectedFramesSliceTrack';

import {NewTrackArgs, TrackBase} from '../../frontend/track';
import {ChromeSliceTrack} from '../chrome_slices';

import {LONG, LONG_NULL, NUM, STR} from '../../common/query_result';
import {duration, time} from '../../base/time';
import {
  TrackController,
} from '../../controller/track_controller';
import {Plugin, PluginContext, PluginDescriptor} from '../../public';
import {BigintMath as BIMath} from '../../base/bigint_math';

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

class ExpectedFramesSliceTrackController extends TrackController<Config, Data> {
  static readonly kind = EXPECTED_FRAMES_SLICE_TRACK_KIND;
  private maxDurNs: duration = 0n;

  async onBoundsChange(start: time, end: time, resolution: duration):
      Promise<Data> {
    if (this.maxDurNs === 0n) {
      const maxDurResult = await this.query(`
        select max(iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts, dur))
          as maxDur
        from experimental_slice_layout
        where filter_track_ids = '${this.config.trackIds.join(',')}'
      `);
      this.maxDurNs = maxDurResult.firstRow({maxDur: LONG_NULL}).maxDur || 0n;
    }

    const queryRes = await this.query(`
      SELECT
        (ts + ${resolution / 2n}) / ${resolution} * ${resolution} as tsq,
        ts,
        max(iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts, dur)) as dur,
        layout_depth as layoutDepth,
        name,
        id,
        dur = 0 as isInstant,
        dur = -1 as isIncomplete
      from experimental_slice_layout
      where
        filter_track_ids = '${this.config.trackIds.join(',')}' and
        ts >= ${start - this.maxDurNs} and
        ts <= ${end}
      group by tsq, layout_depth
      order by tsq, layout_depth
    `);

    const numRows = queryRes.numRows();
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
    const greenIndex = internString('#4CAF50');

    const it = queryRes.iter({
      tsq: LONG,
      ts: LONG,
      dur: LONG,
      layoutDepth: NUM,
      id: NUM,
      name: STR,
      isInstant: NUM,
      isIncomplete: NUM,
    });
    for (let row = 0; it.valid(); it.next(), ++row) {
      const startQ = it.tsq;
      const start = it.ts;
      const dur = it.dur;
      const end = start + dur;
      const minEnd = startQ + resolution;
      const endQ = BIMath.max(BIMath.quant(end, resolution), minEnd);

      slices.starts[row] = startQ;
      slices.ends[row] = endQ;
      slices.depths[row] = it.layoutDepth;
      slices.titles[row] = internString(it.name);
      slices.sliceIds[row] = it.id;
      slices.isInstant[row] = it.isInstant;
      slices.isIncomplete[row] = it.isIncomplete;
      slices.colors![row] = greenIndex;
    }
    return slices;
  }
}


export class ExpectedFramesSliceTrack extends ChromeSliceTrack {
  static readonly kind = EXPECTED_FRAMES_SLICE_TRACK_KIND;
  static create(args: NewTrackArgs): TrackBase {
    return new ExpectedFramesSliceTrack(args);
  }
}

class ExpectedFramesPlugin implements Plugin {
  onActivate(ctx: PluginContext): void {
    ctx.LEGACY_registerTrackController(ExpectedFramesSliceTrackController);
    ctx.LEGACY_registerTrack(ExpectedFramesSliceTrack);
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.ExpectedFrames',
  plugin: ExpectedFramesPlugin,
};
