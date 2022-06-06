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
import {NUM, QueryResult} from '../../common/query_result';
import {fromNs, toNs} from '../../common/time';
import {
  TrackController,
  trackControllerRegistry,
} from '../../controller/track_controller';

import {
  Config,
  Data,
  PROCESS_SCHEDULING_TRACK_KIND,
} from './common';

// This summary is displayed for any processes that have CPU scheduling activity
// associated with them.
class ProcessSchedulingTrackController extends TrackController<Config, Data> {
  static readonly kind = PROCESS_SCHEDULING_TRACK_KIND;

  private maxCpu = 0;
  private maxDurNs = 0;
  private cachedBucketNs = Number.MAX_SAFE_INTEGER;

  async onSetup() {
    await this.createSchedView();

    const cpus = await this.engine.getCpus();

    // A process scheduling track should only exist in a trace that has cpus.
    assertTrue(cpus.length > 0);
    this.maxCpu = Math.max(...cpus) + 1;

    const result = (await this.query(`
      select ifnull(max(dur), 0) as maxDur, count(1) as count
      from ${this.tableName('process_sched')}
    `)).iter({maxDur: NUM, count: NUM});
    assertTrue(result.valid());
    this.maxDurNs = result.maxDur;

    const rowCount = result.count;
    const bucketNs = this.cachedBucketSizeNs(rowCount);
    if (bucketNs === undefined) {
      return;
    }
    await this.query(`
      create table ${this.tableName('process_sched_cached')} as
      select
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as cached_tsq,
        ts,
        max(dur) as dur,
        cpu,
        utid
      from ${this.tableName('process_sched')}
      group by cached_tsq, cpu
      order by cached_tsq, cpu
    `);
    this.cachedBucketNs = bucketNs;
  }

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    assertTrue(this.config.upid !== null);

    // The resolution should always be a power of two for the logic of this
    // function to make sense.
    const resolutionNs = toNs(resolution);
    assertTrue(Math.log2(resolutionNs) % 1 === 0);

    const startNs = toNs(start);
    const endNs = toNs(end);

    // ns per quantization bucket (i.e. ns per pixel). /2 * 2 is to force it to
    // be an even number, so we can snap in the middle.
    const bucketNs =
        Math.max(Math.round(resolutionNs * this.pxSize() / 2) * 2, 1);

    const queryRes = await this.queryData(startNs, endNs, bucketNs);
    const numRows = queryRes.numRows();
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

    const it = queryRes.iter({
      tsq: NUM,
      ts: NUM,
      dur: NUM,
      cpu: NUM,
      utid: NUM,
    });

    for (let row = 0; it.valid(); it.next(), row++) {
      const startNsQ = it.tsq;
      const startNs = it.ts;
      const durNs = it.dur;
      const endNs = startNs + durNs;

      let endNsQ = Math.floor((endNs + bucketNs / 2 - 1) / bucketNs) * bucketNs;
      endNsQ = Math.max(endNsQ, startNsQ + bucketNs);

      slices.starts[row] = fromNs(startNsQ);
      slices.ends[row] = fromNs(endNsQ);
      slices.cpus[row] = it.cpu;
      slices.utids[row] = it.utid;
      slices.end = Math.max(slices.ends[row], slices.end);
    }
    return slices;
  }

  private queryData(startNs: number, endNs: number, bucketNs: number):
      Promise<QueryResult> {
    const isCached = this.cachedBucketNs <= bucketNs;
    const tsq = isCached ? `cached_tsq / ${bucketNs} * ${bucketNs}` :
                           `(ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs}`;
    const queryTable = isCached ? this.tableName('process_sched_cached') :
                                  this.tableName('process_sched');
    const constraintColumn = isCached ? 'cached_tsq' : 'ts';
    return this.query(`
      select
        ${tsq} as tsq,
        ts,
        max(dur) as dur,
        cpu,
        utid
      from ${queryTable}
      where
        ${constraintColumn} >= ${startNs - this.maxDurNs} and
        ${constraintColumn} <= ${endNs}
      group by tsq, cpu
      order by tsq, cpu
    `);
  }

  private async createSchedView() {
    await this.query(`
      create view ${this.tableName('process_sched')} as
      select ts, dur, cpu, utid
      from experimental_sched_upid
      where
        utid != 0 and
        upid = ${this.config.upid}
    `);
  }
}

trackControllerRegistry.register(ProcessSchedulingTrackController);
