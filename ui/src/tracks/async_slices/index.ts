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
import {NUM, NUM_NULL, STR} from '../../common/query_result';
import {fromNs, toNs} from '../../common/time';
import {TrackData} from '../../common/track_data';
import {
  TrackController,
} from '../../controller/track_controller';
import {NewTrackArgs, Track} from '../../frontend/track';
import {ChromeSliceTrack} from '../chrome_slices';

export const ASYNC_SLICE_TRACK_KIND = 'AsyncSliceTrack';

export interface Config {
  maxDepth: number;
  trackIds: number[];
}

export interface Data extends TrackData {
  // Slices are stored in a columnar fashion. All fields have the same length.
  strings: string[];
  sliceIds: Float64Array;
  starts: Float64Array;
  ends: Float64Array;
  depths: Uint16Array;
  titles: Uint16Array;  // Index in |strings|.
  isInstant: Uint16Array;
  isIncomplete: Uint16Array;
}

class AsyncSliceTrackController extends TrackController<Config, Data> {
  static readonly kind = ASYNC_SLICE_TRACK_KIND;
  private maxDurNs = 0;

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const startNs = toNs(start);
    const endNs = toNs(end);

    const pxSize = this.pxSize();

    // ns per quantization bucket (i.e. ns per pixel). /2 * 2 is to force it to
    // be an even number, so we can snap in the middle.
    const bucketNs = Math.max(Math.round(resolution * 1e9 * pxSize / 2) * 2, 1);

    if (this.maxDurNs === 0) {
      const maxDurResult = await this.query(`
        select max(iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts, dur))
        as maxDur from experimental_slice_layout
        where filter_track_ids = '${this.config.trackIds.join(',')}'
      `);
      this.maxDurNs = maxDurResult.firstRow({maxDur: NUM_NULL}).maxDur || 0;
    }

    const queryRes = await this.query(`
      SELECT
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as tsq,
        ts,
        max(iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts, dur)) as dur,
        layout_depth as depth,
        ifnull(name, '[null]') as name,
        id,
        dur = 0 as isInstant,
        dur = -1 as isIncomplete
      from experimental_slice_layout
      where
        filter_track_ids = '${this.config.trackIds.join(',')}' and
        ts >= ${startNs - this.maxDurNs} and
        ts <= ${endNs}
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
      starts: new Float64Array(numRows),
      ends: new Float64Array(numRows),
      depths: new Uint16Array(numRows),
      titles: new Uint16Array(numRows),
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

    const it = queryRes.iter({
      tsq: NUM,
      ts: NUM,
      dur: NUM,
      depth: NUM,
      name: STR,
      id: NUM,
      isInstant: NUM,
      isIncomplete: NUM,
    });
    for (let row = 0; it.valid(); it.next(), row++) {
      const startNsQ = it.tsq;
      const startNs = it.ts;
      const durNs = it.dur;
      const endNs = startNs + durNs;

      let endNsQ = Math.floor((endNs + bucketNs / 2 - 1) / bucketNs) * bucketNs;
      endNsQ = Math.max(endNsQ, startNsQ + bucketNs);

      slices.starts[row] = fromNs(startNsQ);
      slices.ends[row] = fromNs(endNsQ);
      slices.depths[row] = it.depth;
      slices.titles[row] = internString(it.name);
      slices.sliceIds[row] = it.id;
      slices.isInstant[row] = it.isInstant;
      slices.isIncomplete[row] = it.isIncomplete;
    }
    return slices;
  }
}

export class AsyncSliceTrack extends ChromeSliceTrack {
  static readonly kind = ASYNC_SLICE_TRACK_KIND;
  static create(args: NewTrackArgs): Track {
    return new AsyncSliceTrack(args);
  }
}

function activate(ctx: PluginContext) {
  ctx.registerTrackController(AsyncSliceTrackController);
  ctx.registerTrack(AsyncSliceTrack);
}

export const plugin = {
  pluginId: 'perfetto.AsyncSlices',
  activate,
};
