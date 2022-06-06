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

import {NUM} from '../../common/query_result';
import {
  TrackController,
  trackControllerRegistry,
} from '../../controller/track_controller';

import {
  Config,
  Data,
  PERF_SAMPLES_PROFILE_TRACK_KIND,
} from './common';

class PerfSamplesProfileTrackController extends TrackController<Config, Data> {
  static readonly kind = PERF_SAMPLES_PROFILE_TRACK_KIND;
  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    if (this.config.upid === undefined) {
      return {
        start,
        end,
        resolution,
        length: 0,
        tsStartsNs: new Float64Array(),
      };
    }
    const queryRes = await this.query(`
     select ts, upid from perf_sample
     join thread using (utid)
     where upid = ${this.config.upid}
     order by ts`);
    const numRows = queryRes.numRows();
    const data: Data = {
      start,
      end,
      resolution,
      length: numRows,
      tsStartsNs: new Float64Array(numRows),
    };

    const it = queryRes.iter({ts: NUM});
    for (let row = 0; it.valid(); it.next(), row++) {
      data.tsStartsNs[row] = it.ts;
    }
    return data;
  }
}

trackControllerRegistry.register(PerfSamplesProfileTrackController);
