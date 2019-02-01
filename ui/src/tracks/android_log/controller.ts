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
import {
  TrackController,
  trackControllerRegistry
} from '../../controller/track_controller';

import {ANDROID_LOGS_TRACK_KIND, Config, Data} from './common';

class AndroidLogTrackController extends TrackController<Config, Data> {
  static readonly kind = ANDROID_LOGS_TRACK_KIND;
  private busy = false;

  onBoundsChange(start: number, end: number, resolution: number) {
    this.update(start, end, resolution);
  }

  private async update(start: number, end: number, resolution: number) {
    // TODO(hjd): we should really call TraceProcessor.Interrupt() here.
    if (this.busy) return;
    this.busy = true;

    const startNs = Math.floor(start * 1e9);
    const endNs = Math.ceil(end * 1e9);

    // |resolution| is in s/px the frontend wants.
    const quantNs = Math.ceil(resolution * 1e9);

    const rawResult = await this.engine.query(`
      select
        cast(ts / ${quantNs} as integer) * ${quantNs} as ts_quant,
        prio,
        count(prio)
      from android_logs
      where ts >= ${startNs} and ts <= ${endNs}
      group by ts_quant, prio
      order by ts_quant, prio;`);
    this.busy = false;

    const rowCount = +rawResult.numRecords;
    const result = {
      start,
      end,
      resolution,
      numEvents: 0,
      timestamps: new Float64Array(rowCount),
      priorities: new Uint8Array(rowCount),
    };
    const cols = rawResult.columns;
    for (let i = 0; i < rowCount; i++) {
      result.timestamps[i] = fromNs(+cols[0].longValues![i]);
      const prio = Math.min(+cols[1].longValues![i], 7);
      result.priorities[i] |= (1 << prio);
      result.numEvents += +cols[2].longValues![i];
    }
    this.publish(result);
  }
}

trackControllerRegistry.register(AndroidLogTrackController);
