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

import {assertTrue} from '../../base/logging';
import {NUM} from '../../common/query_result';
import {fromNs, toNs} from '../../common/time';
import {
  TrackController,
  trackControllerRegistry
} from '../../controller/track_controller';

import {Config, CPU_SLICE_TRACK_KIND, Data} from './common';

class CpuSliceTrackController extends TrackController<Config, Data> {
  static readonly kind = CPU_SLICE_TRACK_KIND;

  private cachedBucketNs = Number.MAX_SAFE_INTEGER;
  private maxDurNs = 0;
  private lastRowId = -1;

  async onSetup() {
    await this.query(`
      create view ${this.tableName('sched')} as
      select
        ts,
        dur,
        utid,
        id,
        dur = -1 as isIncomplete
      from sched
      where cpu = ${this.config.cpu} and utid != 0
    `);

    const queryRes = await this.query(`
      select ifnull(max(dur), 0) as maxDur, count(1) as rowCount
      from ${this.tableName('sched')}
    `);

    const queryLastSlice = await this.query(`
    select max(id) as lastSliceId from ${this.tableName('sched')}
    `);
    this.lastRowId = queryLastSlice.firstRow({lastSliceId: NUM}).lastSliceId;

    const row = queryRes.firstRow({maxDur: NUM, rowCount: NUM});
    this.maxDurNs = row.maxDur;
    const rowCount = row.rowCount;
    const bucketNs = this.cachedBucketSizeNs(rowCount);
    if (bucketNs === undefined) {
      return;
    }

    await this.query(`
      create table ${this.tableName('sched_cached')} as
      select
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as cached_tsq,
        ts,
        max(dur) as dur,
        utid,
        id,
        isIncomplete
      from ${this.tableName('sched')}
      group by cached_tsq, isIncomplete
      order by cached_tsq
    `);
    this.cachedBucketNs = bucketNs;
  }

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const resolutionNs = toNs(resolution);

    // The resolution should always be a power of two for the logic of this
    // function to make sense.
    assertTrue(Math.log2(resolutionNs) % 1 === 0);

    const boundStartNs = toNs(start);
    const boundEndNs = toNs(end);

    // ns per quantization bucket (i.e. ns per pixel). /2 * 2 is to force it to
    // be an even number, so we can snap in the middle.
    const bucketNs =
        Math.max(Math.round(resolutionNs * this.pxSize() / 2) * 2, 1);

    const isCached = this.cachedBucketNs <= bucketNs;
    const queryTsq = isCached ?
        `cached_tsq / ${bucketNs} * ${bucketNs}` :
        `(ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs}`;
    const queryTable =
        isCached ? this.tableName('sched_cached') : this.tableName('sched');
    const constraintColumn = isCached ? 'cached_tsq' : 'ts';

    const queryRes = await this.query(`
      select
        ${queryTsq} as tsq,
        ts,
        max(dur) as dur,
        utid,
        id,
        isIncomplete
      from ${queryTable}
      where
        ${constraintColumn} >= ${boundStartNs - this.maxDurNs} and
        ${constraintColumn} <= ${boundEndNs}
      group by tsq, isIncomplete
      order by tsq
    `);

    const numRows = queryRes.numRows();
    const slices: Data = {
      start,
      end,
      resolution,
      length: numRows,
      lastRowId: this.lastRowId,
      ids: new Float64Array(numRows),
      starts: new Float64Array(numRows),
      ends: new Float64Array(numRows),
      utids: new Uint32Array(numRows),
      isIncomplete: new Uint8Array(numRows)
    };

    const it = queryRes.iter(
        {tsq: NUM, ts: NUM, dur: NUM, utid: NUM, id: NUM, isIncomplete: NUM});
    for (let row = 0; it.valid(); it.next(), row++) {
      const startNsQ = it.tsq;
      const startNs = it.ts;
      const durNs = it.dur;
      const endNs = startNs + durNs;

      // If the slice is incomplete, the end calculated later.
      if (!it.isIncomplete) {
        let endNsQ =
            Math.floor((endNs + bucketNs / 2 - 1) / bucketNs) * bucketNs;
        endNsQ = Math.max(endNsQ, startNsQ + bucketNs);
        slices.ends[row] = fromNs(endNsQ);
      }

      slices.starts[row] = fromNs(startNsQ);
      slices.utids[row] = it.utid;
      slices.ids[row] = it.id;
      slices.isIncomplete[row] = it.isIncomplete;
    }

    // If the slice is incomplete and it is the last slice in the track, the end
    // of the slice would be the end of the visible window. Otherwise we end the
    // slice with the beginning the next one.
    for (let row = 0; row < slices.length; row++) {
      if (!slices.isIncomplete[row]) {
        continue;
      }
      const endNs =
          row === slices.length - 1 ? boundEndNs : toNs(slices.starts[row + 1]);

      let endNsQ = Math.floor((endNs + bucketNs / 2 - 1) / bucketNs) * bucketNs;
      endNsQ = Math.max(endNsQ, toNs(slices.starts[row]) + bucketNs);
      slices.ends[row] = fromNs(endNsQ);
    }
    return slices;
  }

  async onDestroy() {
    await this.query(`drop table if exists ${this.tableName('sched_cached')}`);
  }
}

trackControllerRegistry.register(CpuSliceTrackController);
