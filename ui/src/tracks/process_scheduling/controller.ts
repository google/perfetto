// Copyright (C) 2019 The Android Open Source Project
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

import {assertTrue} from '../../base/logging';
import {fromNs, toNs} from '../../common/time';
import {
  TrackController,
  trackControllerRegistry
} from '../../controller/track_controller';

import {
  Config,
  Data,
  PROCESS_SCHEDULING_TRACK_KIND,
} from './common';


// Allow to override via devtools for testing (note, needs to be done in the
// controller-thread).
(self as {} as {quantPx: number}).quantPx = 1;

// This summary is displayed for any processes that have CPU scheduling activity
// associated with them.

class ProcessSchedulingTrackController extends TrackController<Config, Data> {
  static readonly kind = PROCESS_SCHEDULING_TRACK_KIND;
  private setup = false;
  private maxDurNs = 0;
  private maxCpu = 0;

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    if (this.config.upid === null) {
      throw new Error('Upid not set.');
    }

    const startNs = toNs(start);
    const endNs = toNs(end);

    const pxSize = (self as {} as {quantPx: number}).quantPx;

    // ns per quantization bucket (i.e. ns per pixel). /2 * 2 is to force it to
    // be an even number, so we can snap in the middle.
    const bucketNs = Math.round(resolution * 1e9 * pxSize / 2) * 2;

    if (this.setup === false) {
      const cpus = await this.engine.getCpus();
      // A process scheduling track should only exist in a trace that has cpus.
      assertTrue(cpus.length > 0);
      this.maxCpu = Math.max(...cpus) + 1;

      const maxDurResult = await this.query(`select max(dur)
        from sched
        join thread using(utid)
        where
          utid != 0 and
          upid = ${this.config.upid}`);
      if (maxDurResult.numRecords === 1) {
        this.maxDurNs = +maxDurResult.columns![0].longValues![0];
      }

      this.setup = true;
    }

    const rawResult = await this.query(`select
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as tsq,
        ts,
        max(dur) as dur,
        cpu,
        utid
      from sched
      join thread using(utid)
      where
        ts >= ${startNs - this.maxDurNs} and
        ts <= ${endNs} and
        utid != 0 and
        upid = ${this.config.upid}
      group by cpu, tsq`);

    const numRows = +rawResult.numRecords;
    const slices: Data = {
      kind: 'slice',
      start,
      end,
      resolution,
      length: numRows,
      maxCpu: this.maxCpu,
      starts: new Float64Array(numRows),
      ends: new Float64Array(numRows),
      cpus: new Uint32Array(numRows),
      utids: new Uint32Array(numRows),
    };

    const cols = rawResult.columns;
    for (let row = 0; row < numRows; row++) {
      const startNsQ = +cols[0].longValues![row];
      const startNs = +cols[1].longValues![row];
      const durNs = +cols[2].longValues![row];
      const endNs = startNs + durNs;

      let endNsQ = Math.floor((endNs + bucketNs / 2 - 1) / bucketNs) * bucketNs;
      endNsQ = Math.max(endNsQ, startNsQ + bucketNs);

      if (startNsQ === endNsQ) {
        throw new Error('Should never happen');
      }

      slices.starts[row] = fromNs(startNsQ);
      slices.ends[row] = fromNs(endNsQ);
      slices.cpus[row] = +cols[3].longValues![row];
      slices.utids[row] = +cols[4].longValues![row];
      slices.end = Math.max(slices.ends[row], slices.end);
    }
    return slices;
  }
}

trackControllerRegistry.register(ProcessSchedulingTrackController);
