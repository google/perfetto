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

import {fromNs} from '../../common/time';
import {
  TrackController,
  trackControllerRegistry
} from '../../controller/track_controller';

import {
  Config,
  Data,
  PROCESS_SCHEDULING_TRACK_KIND,
  SliceData,
  SummaryData
} from './common';

class ProcessSchedulingTrackController extends TrackController<Config, Data> {
  static readonly kind = PROCESS_SCHEDULING_TRACK_KIND;
  private busy = false;
  private setup = false;
  private numCpus = 0;

  onBoundsChange(start: number, end: number, resolution: number): void {
    this.update(start, end, resolution);
  }

  private async update(start: number, end: number, resolution: number):
      Promise<void> {
    // TODO: we should really call TraceProcessor.Interrupt() at this point.
    if (this.busy) return;

    if (!this.config.upid) {
      return;
    }

    const startNs = Math.round(start * 1e9);
    const endNs = Math.round(end * 1e9);

    this.busy = true;
    if (this.setup === false) {
      await this.query(
          `create virtual table ${this.tableName('window')} using window;`);
      await this.query(`create view ${this.tableName('process')} as
          select ts, dur, cpu, utid, upid from sched join (
          select utid, upid from thread
            where utid != 0 and upid = ${this.config.upid}) using(utid);`);
      await this.query(`create virtual table ${this.tableName('span')}
              using span_join(${this.tableName('process')} PARTITIONED cpu,
                              ${this.tableName('window')} PARTITIONED cpu);`);
      this.numCpus = await this.engine.getNumberOfCpus();
      this.setup = true;
    }

    const isQuantized = this.shouldSummarize(resolution);
    // |resolution| is in s/px we want # ns for 20px window:
    const bucketSizeNs = Math.round(resolution * 10 * 1e9 * 1.5);
    let windowStartNs = startNs;
    if (isQuantized) {
      windowStartNs = Math.floor(windowStartNs / bucketSizeNs) * bucketSizeNs;
    }
    const windowDurNs = Math.max(1, endNs - windowStartNs);

    this.query(`update ${this.tableName('window')} set
      window_start=${windowStartNs},
      window_dur=${windowDurNs},
      quantum=${isQuantized ? bucketSizeNs : 0}
      where rowid = 0;`);

    if (isQuantized) {
      this.publish(await this.computeSummary(
          fromNs(windowStartNs), end, resolution, bucketSizeNs));
    } else {
      this.publish(
          await this.computeSlices(fromNs(windowStartNs), end, resolution));
    }
    this.busy = false;
  }

  private async computeSummary(
      start: number, end: number, resolution: number,
      bucketSizeNs: number): Promise<SummaryData> {
    const startNs = Math.round(start * 1e9);
    const endNs = Math.round(end * 1e9);
    const numBuckets = Math.ceil((endNs - startNs) / bucketSizeNs);

    // cpu < numCpus improves perfomance a lot since the window table can
    // avoid generating many rows.
    const query = `select
        quantum_ts as bucket,
        sum(dur)/cast(${bucketSizeNs * this.numCpus} as float) as utilization
        from ${this.tableName('span')}
        where upid = ${this.config.upid}
        and utid != 0
        and cpu < ${this.numCpus}
        group by quantum_ts`;

    const rawResult = await this.query(query);
    const numRows = +rawResult.numRecords;

    const summary: Data = {
      kind: 'summary',
      start,
      end,
      resolution,
      bucketSizeSeconds: fromNs(bucketSizeNs),
      utilizations: new Float64Array(numBuckets),
    };
    const cols = rawResult.columns;
    for (let row = 0; row < numRows; row++) {
      const bucket = +cols[0].longValues![row];
      summary.utilizations[bucket] = +cols[1].doubleValues![row];
    }
    return summary;
  }

  private async computeSlices(start: number, end: number, resolution: number):
      Promise<SliceData> {
    // TODO(hjd): Remove LIMIT
    const LIMIT = 10000;

    // cpu < numCpus improves perfomance a lot since the window table can
    // avoid generating many rows.
    const query = `select ts,dur,cpu,utid from ${this.tableName('span')}
        join
        (select utid from thread where upid = ${this.config.upid})
        using(utid)
        where
        cpu < ${this.numCpus}
        order by
        cpu,
        ts
        limit ${LIMIT};`;
    const rawResult = await this.query(query);

    const numRows = +rawResult.numRecords;
    if (numRows === LIMIT) {
      console.warn(`More than ${LIMIT} rows returned.`);
    }
    const slices: SliceData = {
      kind: 'slice',
      start,
      end,
      resolution,
      numCpus: this.numCpus,
      starts: new Float64Array(numRows),
      ends: new Float64Array(numRows),
      cpus: new Uint32Array(numRows),
      utids: new Uint32Array(numRows),
    };

    const cols = rawResult.columns;
    for (let row = 0; row < numRows; row++) {
      const startSec = fromNs(+cols[0].longValues![row]);
      slices.starts[row] = startSec;
      slices.ends[row] = startSec + fromNs(+cols[1].longValues![row]);
      slices.cpus[row] = +cols[2].longValues![row];
      slices.utids[row] = +cols[3].longValues![row];
      slices.end = Math.max(slices.ends[row], slices.end);
    }
    return slices;
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
      this.query(`drop table ${this.tableName('window')}`);
      this.query(`drop table ${this.tableName('span')}`);
      this.setup = false;
    }
  }
}

trackControllerRegistry.register(ProcessSchedulingTrackController);
