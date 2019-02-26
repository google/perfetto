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

import {Config, Data, KIND} from './common';

class VsyncTrackController extends TrackController<Config, Data> {
  static readonly kind = KIND;
  private busy = false;
  private setup = false;

  onBoundsChange(start: number, end: number, resolution: number) {
    this.update(start, end, resolution);
  }

  private async update(start: number, end: number, resolution: number) {
    // TODO(hjd): we should really call TraceProcessor.Interrupt() here.
    if (this.busy) return;
    this.busy = true;

    if (this.setup === false) {
      await this.query(
          `create virtual table window_${this.trackState.id} using window;`);
      await this.query(
          `create virtual table span_${this.trackState.id}
              using span_join(sched PARTITIONED cpu,
                              window_${this.trackState.id});`);
      this.setup = true;
    }

    const rawResult = await this.engine.query(`
      select ts from counters
        where name like "${this.config.counterName}%"
        order by ts;`);
    this.busy = false;
    const rowCount = +rawResult.numRecords;
    const result = {
      start,
      end,
      resolution,
      vsyncs: new Float64Array(rowCount),
    };
    const cols = rawResult.columns;
    for (let i = 0; i < rowCount; i++) {
      const startSec = fromNs(+cols[0].longValues![i]);
      result.vsyncs[i] = startSec;
    }
    this.publish(result);
  }

  private async query(query: string) {
    const result = await this.engine.query(query);
    if (result.error) {
      console.error(`Query error "${query}": ${result.error}`);
      throw new Error(`Query error "${query}": ${result.error}`);
    }
    return result;
  }

  onDestroy(): void {
    if (this.setup) {
      this.query(`drop table window_${this.trackState.id}`);
      this.query(`drop table span_${this.trackState.id}`);
      this.setup = false;
    }
  }
}

trackControllerRegistry.register(VsyncTrackController);
