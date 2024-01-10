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
import {Duration, duration, time} from '../../base/time';
import {SliceData, SliceTrackLEGACY} from '../../frontend/slice_track';
import {EngineProxy} from '../../public';
import {
  LONG,
  LONG_NULL,
  NUM,
  STR,
} from '../../trace_processor/query_result';

export class ExpectedFramesTrack extends SliceTrackLEGACY {
  private maxDur = Duration.ZERO;

  constructor(
      private engine: EngineProxy, maxDepth: number, trackKey: string,
      private trackIds: number[], namespace?: string) {
    super(maxDepth, trackKey, '', namespace);
  }

  async onBoundsChange(start: time, end: time, resolution: duration):
      Promise<SliceData> {
    if (this.maxDur === Duration.ZERO) {
      const maxDurResult = await this.engine.query(`
        select max(iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts, dur))
          as maxDur
        from experimental_slice_layout
        where filter_track_ids = '${this.trackIds.join(',')}'
      `);
      this.maxDur = maxDurResult.firstRow({maxDur: LONG_NULL}).maxDur ?? 0n;
    }

    const queryRes = await this.engine.query(`
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
        filter_track_ids = '${this.trackIds.join(',')}' and
        ts >= ${start - this.maxDur} and
        ts <= ${end}
      group by tsq, layout_depth
      order by tsq, layout_depth
    `);

    const numRows = queryRes.numRows();
    const slices: SliceData = {
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
