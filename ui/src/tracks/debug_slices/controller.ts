// Copyright (C) 2020 The Android Open Source Project
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

import {Actions} from '../../common/actions';
import {NUM, NUM_NULL, STR} from '../../common/query_result';
import {fromNs, toNs} from '../../common/time';
import {globals} from '../../controller/globals';
import {
  TrackController,
  trackControllerRegistry,
} from '../../controller/track_controller';

import {Config, Data, DEBUG_SLICE_TRACK_KIND} from './common';

class DebugSliceTrackController extends TrackController<Config, Data> {
  static readonly kind = DEBUG_SLICE_TRACK_KIND;

  async onReload() {
    const rawResult = await this.query(
        `select ifnull(max(depth), 1) as maxDepth from debug_slices`);
    const maxDepth = rawResult.firstRow({maxDepth: NUM}).maxDepth;
    globals.dispatch(
        Actions.updateTrackConfig({id: this.trackId, config: {maxDepth}}));
  }

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const queryRes = await this.query(`select
      ifnull(id, -1) as id,
      ifnull(name, '[null]') as name,
      ts,
      iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts, dur) as dur,
      ifnull(depth, 0) as depth
      from debug_slices
      where (ts + dur) >= ${toNs(start)} and ts <= ${toNs(end)}`);

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

    const it = queryRes.iter(
        {id: NUM, name: STR, ts: NUM_NULL, dur: NUM_NULL, depth: NUM});
    for (let row = 0; it.valid(); it.next(), row++) {
      let sliceStart: number;
      let sliceEnd: number;
      if (it.ts === null || it.dur === null) {
        sliceStart = sliceEnd = -1;
      } else {
        sliceStart = it.ts;
        sliceEnd = sliceStart + it.dur;
      }
      slices.sliceIds[row] = it.id;
      slices.starts[row] = fromNs(sliceStart);
      slices.ends[row] = fromNs(sliceEnd);
      slices.depths[row] = it.depth;
      const sliceName = it.name;
      slices.titles[row] = internString(sliceName);
      slices.isInstant[row] = 0;
      slices.isIncomplete[row] = 0;
    }

    return slices;
  }
}

trackControllerRegistry.register(DebugSliceTrackController);
