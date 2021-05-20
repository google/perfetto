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

import {assertExists} from '../../base/logging';
import {
  iter,
  NUM,
  singleRow,
  slowlyCountRows,
  STR
} from '../../common/query_iterator';
import {fromNs, toNs} from '../../common/time';
import {
  TrackController,
  trackControllerRegistry,
} from '../../controller/track_controller';

import {Config, Data, EXPECTED_FRAMES_SLICE_TRACK_KIND} from './common';

class ExpectedFramesSliceTrackController extends TrackController<Config, Data> {
  static readonly kind = EXPECTED_FRAMES_SLICE_TRACK_KIND;
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
          as maxDur
        from experimental_slice_layout
        where filter_track_ids = '${this.config.trackIds.join(',')}'
      `);
      const row = singleRow({maxDur: NUM}, maxDurResult);
      this.maxDurNs = assertExists(row).maxDur;
    }

    const rawResult = await this.query(`
      SELECT
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as tsq,
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
        ts >= ${startNs - this.maxDurNs} and
        ts <= ${endNs}
      group by tsq, layout_depth
      order by tsq, layout_depth
    `);

    const numRows = slowlyCountRows(rawResult);
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

    const it = iter(
        {
          tsq: NUM,
          ts: NUM,
          dur: NUM,
          layoutDepth: NUM,
          id: NUM,
          name: STR,
          isInstant: NUM,
          isIncomplete: NUM,
        },
        rawResult);
    for (let i = 0; it.valid(); it.next(), ++i) {
      const startNsQ = it.row.tsq;
      const startNs = it.row.ts;
      const durNs = it.row.dur;
      const endNs = startNs + durNs;

      let endNsQ = Math.floor((endNs + bucketNs / 2 - 1) / bucketNs) * bucketNs;
      endNsQ = Math.max(endNsQ, startNsQ + bucketNs);

      if (startNsQ === endNsQ) {
        throw new Error('Should never happen');
      }

      slices.starts[i] = fromNs(startNsQ);
      slices.ends[i] = fromNs(endNsQ);
      slices.depths[i] = it.row.layoutDepth;
      slices.titles[i] = internString(it.row.name);
      slices.sliceIds[i] = it.row.id;
      slices.isInstant[i] = it.row.isInstant;
      slices.isIncomplete[i] = it.row.isIncomplete;
      slices.colors![i] = greenIndex;
    }
    return slices;
  }
}


trackControllerRegistry.register(ExpectedFramesSliceTrackController);
