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

  async onSetup() {
    await this.queryV2(`
      create view ${this.tableName('sched')} as
      select
        ts,
        dur,
        utid,
        id
      from sched
      where cpu = ${this.config.cpu} and utid != 0
    `);

    const queryRes = await this.queryV2(`
      select ifnull(max(dur), 0) as maxDur, count(1) as rowCount
      from ${this.tableName('sched')}
    `);
    const row = queryRes.firstRow({maxDur: NUM, rowCount: NUM});
    this.maxDurNs = row.maxDur;
    const rowCount = row.rowCount;
    const bucketNs = this.cachedBucketSizeNs(rowCount);
    if (bucketNs === undefined) {
      return;
    }
    await this.queryV2(`
      create table ${this.tableName('sched_cached')} as
      select
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as cached_tsq,
        ts,
        max(dur) as dur,
        utid,
        id
      from ${this.tableName('sched')}
      group by cached_tsq
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

    const startNs = toNs(start);
    const endNs = toNs(end);

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

    const queryRes = await this.queryV2(`
      select
        ${queryTsq} as tsq,
        ts,
        max(dur) as dur,
        utid,
        id
      from ${queryTable}
      where
        ${constraintColumn} >= ${startNs - this.maxDurNs} and
        ${constraintColumn} <= ${endNs}
      group by tsq
      order by tsq
    `);

    const numRows = queryRes.numRows();
    const slices: Data = {
      start,
      end,
      resolution,
      length: numRows,
      ids: new Float64Array(numRows),
      starts: new Float64Array(numRows),
      ends: new Float64Array(numRows),
      utids: new Uint32Array(numRows),
    };

    const it = queryRes.iter({tsq: NUM, ts: NUM, dur: NUM, utid: NUM, id: NUM});
    for (let row = 0; it.valid(); it.next(), row++) {
      const startNsQ = it.tsq;
      const startNs = it.ts;
      const durNs = it.dur;
      const endNs = startNs + durNs;

      let endNsQ = Math.floor((endNs + bucketNs / 2 - 1) / bucketNs) * bucketNs;
      endNsQ = Math.max(endNsQ, startNsQ + bucketNs);

      slices.starts[row] = fromNs(startNsQ);
      slices.ends[row] = fromNs(endNsQ);
      slices.utids[row] = it.utid;
      slices.ids[row] = it.id;
    }

    return slices;
  }

  async onDestroy() {
    await this.queryV2(
        `drop table if exists ${this.tableName('sched_cached')}`);
  }
}

trackControllerRegistry.register(CpuSliceTrackController);
