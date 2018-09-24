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

import {Config, CPU_SLICE_TRACK_KIND, Data} from './common';

class CpuSliceTrackController extends TrackController<Config, Data> {
  static readonly kind = CPU_SLICE_TRACK_KIND;
  private busy = false;
  private setup = false;

  onBoundsChange(start: number, end: number, resolution: number): void {
    this.update(start, end, resolution);
  }

  private async update(start: number, end: number, resolution: number):
      Promise<void> {
    // TODO: we should really call TraceProcessor.Interrupt() at this point.
    if (this.busy) return;

    const startNs = Math.round(start * 1e9);
    const endNs = Math.round(end * 1e9);
    const resolutionNs = Math.round(resolution * 1e9);

    this.busy = true;
    if (this.setup === false) {
      await this.query(
          `create virtual table window_${this.trackState.id} using window;`);
      await this.query(`create virtual table span_${this.trackState.id}
                     using span(sched, window_${this.trackState.id}, cpu);`);
      this.setup = true;
    }

    this.query(`update window_${this.trackState.id} set
      window_start=${startNs},
      window_dur=${endNs - startNs}
      where rowid = 0;`);

    const LIMIT = 10000;
    const query = `select ts,dur,utid from span_${this.trackState.id} 
        where cpu = ${this.config.cpu}
        and utid != 0
        and dur >= ${resolutionNs}
        limit ${LIMIT};`;
    const rawResult = await this.query(query);

    const numRows = +rawResult.numRecords;

    const slices: Data = {
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
    this.publish(slices);
    this.busy = false;
  }

  private async query(query: string) {
    const result = await this.engine.query(query);
    if (result.error) {
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

trackControllerRegistry.register(CpuSliceTrackController);
