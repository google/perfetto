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

import {assertExists, assertTrue} from '../../base/logging';
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

import {ACTUAL_FRAMES_SLICE_TRACK_KIND, Config, Data} from './common';

const BLUE_COLOR = '#03A9F4';    // Blue 500
const GREEN_COLOR = '#4CAF50';     // Green 500
const YELLOW_COLOR = '#FFEB3B';  // Yellow 500
const RED_COLOR = '#FF5722';      // Red 500
const LIGHT_GREEN_COLOR = '#C0D588'; // Light Green 500
const PINK_COLOR = '#F515E0';        // Pink 500

class ActualFramesSliceTrackController extends TrackController<Config, Data> {
  static readonly kind = ACTUAL_FRAMES_SLICE_TRACK_KIND;
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
        select
          max(iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts, dur))
            as maxDur
        from experimental_slice_layout
        where filter_track_ids = '${this.config.trackIds.join(',')}'
      `);
      const row = singleRow({maxDur: NUM}, maxDurResult);
      this.maxDurNs = assertExists(row).maxDur;
    }

    const rawResult = await this.query(`
      SELECT
        (s.ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as tsq,
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
        s.ts >= ${startNs - this.maxDurNs} and
        s.ts <= ${endNs}
      group by tsq, s.layout_depth
      order by tsq, s.layout_depth
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

    const it = iter(
        {
          'tsq': NUM,
          'ts': NUM,
          'dur': NUM,
          'layoutDepth': NUM,
          'id': NUM,
          'name': STR,
          'isInstant': NUM,
          'isIncomplete': NUM,
          'color': STR,
        },
        rawResult);
    for (let i = 0; it.valid(); i++, it.next()) {
      const startNsQ = it.row.tsq;
      const startNs = it.row.ts;
      const durNs = it.row.dur;
      const endNs = startNs + durNs;

      let endNsQ = Math.floor((endNs + bucketNs / 2 - 1) / bucketNs) * bucketNs;
      endNsQ = Math.max(endNsQ, startNsQ + bucketNs);

      assertTrue(startNsQ !== endNsQ);

      slices.starts[i] = fromNs(startNsQ);
      slices.ends[i] = fromNs(endNsQ);
      slices.depths[i] = it.row.layoutDepth;
      slices.titles[i] = internString(it.row.name);
      slices.colors![i] = internString(it.row.color);
      slices.sliceIds[i] = it.row.id;
      slices.isInstant[i] = it.row.isInstant;
      slices.isIncomplete[i] = it.row.isIncomplete;
    }
    return slices;
  }
}


trackControllerRegistry.register(ActualFramesSliceTrackController);
