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

import {slowlyCountRows} from '../../common/query_iterator';
import {
  TrackController,
  trackControllerRegistry
} from '../../controller/track_controller';

import {
  Config,
  CPU_PROFILE_TRACK_KIND,
  Data,
} from './common';

class CpuProfileTrackController extends TrackController<Config, Data> {
  static readonly kind = CPU_PROFILE_TRACK_KIND;
  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const query = `select id, ts from cpu_profile_stack_sample
        where utid = ${this.config.utid}
        order by ts`;

    const result = await this.query(query);

    const numRows = slowlyCountRows(result);
    const data: Data = {
      start,
      end,
      resolution,
      length: numRows,
      ids: new Float64Array(numRows),
      tsStarts: new Float64Array(numRows),
    };

    for (let row = 0; row < numRows; row++) {
      data.ids[row] = +result.columns[0].longValues![row];
      data.tsStarts[row] = +result.columns[1].longValues![row];
    }

    return data;
  }
}

trackControllerRegistry.register(CpuProfileTrackController);
