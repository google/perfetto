// Copyright (C) 2018 The Android Open Source Project
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

import {fromNs} from '../../common/time';
import {globals} from '../../controller/globals';
import {
  TrackController,
  trackControllerRegistry
} from '../../controller/track_controller';

import {
  ChromeSliceTrackConfig,
  ChromeSliceTrackData,
  SLICE_TRACK_KIND
} from './common';

class ChromeSliceTrackController extends
    TrackController<ChromeSliceTrackConfig> {
  static readonly kind = SLICE_TRACK_KIND;
  private busy = false;

  onBoundsChange(start: number, end: number, resolution: number) {
    // TODO: we should really call TraceProcessor.Interrupt() at this point.
    if (this.busy) return;
    const LIMIT = 10000;

    // TODO: "ts >= x - dur" below is inefficient because doesn't allow to use
    // any index. We need to introduce ts_lower_bound also for the slices table
    // (see sched table).
    const query = `select ts,dur,depth,cat,name from slices ` +
        `where utid = ${this.config.utid} ` +
        `and ts >= ${Math.round(start * 1e9)} - dur ` +
        `and ts <= ${Math.round(end * 1e9)} ` +
        `and dur >= ${Math.round(resolution * 1e9)} ` +
        `order by ts ` +
        `limit ${LIMIT};`;

    this.busy = true;
    this.engine.rawQuery({'sqlQuery': query}).then(rawResult => {
      this.busy = false;
      if (rawResult.error) {
        throw new Error(`Query error "${query}": ${rawResult.error}`);
      }

      const numRows = +rawResult.numRecords;

      const slices: ChromeSliceTrackData = {
        start,
        end,
        resolution,
        strings: [],
        starts: new Float64Array(numRows),
        ends: new Float64Array(numRows),
        depths: new Uint16Array(numRows),
        titles: new Uint16Array(numRows),
        categories: new Uint16Array(numRows),
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

      for (let row = 0; row < numRows; row++) {
        const cols = rawResult.columns;
        const startSec = fromNs(+cols[0].longValues![row]);
        slices.starts[row] = startSec;
        slices.ends[row] = startSec + fromNs(+cols[1].longValues![row]);
        slices.depths[row] = +cols[2].longValues![row];
        slices.categories[row] = internString(cols[3].stringValues![row]);
        slices.titles[row] = internString(cols[4].stringValues![row]);
      }
      if (numRows === LIMIT) {
        slices.end = slices.ends[slices.ends.length - 1];
      }
      globals.publish('TrackData', {id: this.trackId, data: slices});
    });
  }
}


trackControllerRegistry.register(ChromeSliceTrackController);
