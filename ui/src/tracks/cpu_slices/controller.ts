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
  CPU_SLICE_TRACK_KIND,
  CpuSliceTrackConfig,
  CpuSliceTrackData
} from './common';

class CpuSliceTrackController extends TrackController<CpuSliceTrackConfig> {
  static readonly kind = CPU_SLICE_TRACK_KIND;
  private busy = false;

  onBoundsChange(start: number, end: number, resolution: number) {
    // TODO: we should really call TraceProcessor.Interrupt() at this point.
    if (this.busy) return;
    const LIMIT = 10000;
    const query = 'select ts,dur,utid from sched ' +
        `where cpu = ${this.config.cpu} ` +
        `and ts_lower_bound = ${Math.round(start * 1e9)} ` +
        `and ts <= ${Math.round(end * 1e9)} ` +
        `and dur >= ${Math.round(resolution * 1e9)} ` +
        `and utid != 0 ` +
        `order by ts ` +
        `limit ${LIMIT};`;

    this.busy = true;
    this.engine.rawQuery({'sqlQuery': query}).then(rawResult => {
      this.busy = false;
      if (rawResult.error) {
        throw new Error(`Query error "${query}": ${rawResult.error}`);
      }
      const numRows = +rawResult.numRecords;

      const slices: CpuSliceTrackData = {
        start,
        end,
        resolution,
        starts: new Float64Array(numRows),
        ends: new Float64Array(numRows),
        utids: new Uint32Array(numRows),
      };

      for (let row = 0; row < numRows; row++) {
        const cols = rawResult.columns;
        const startSec = fromNs(+cols[0].longValues![row]);
        slices.starts[row] = startSec;
        slices.ends[row] = startSec + fromNs(+cols[1].longValues![row]);
        slices.utids[row] = +cols[2].longValues![row];
      }
      if (numRows === LIMIT) {
        slices.end = slices.ends[slices.ends.length - 1];
      }
      globals.publish('TrackData', {id: this.trackId, data: slices});
    });
  }
}

trackControllerRegistry.register(CpuSliceTrackController);
