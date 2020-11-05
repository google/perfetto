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
import {slowlyCountRows} from '../../common/query_iterator';
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
    await this.query(`
      create view ${this.tableName('sched')} as
      select
        ts,
        dur,
        utid,
        id
      from sched
      where cpu = ${this.config.cpu} and utid != 0
    `);

    const rawResult = await this.query(`
      select max(dur), count(1)
      from ${this.tableName('sched')}
    `);
    this.maxDurNs = rawResult.columns[0].longValues![0];

    const rowCount = rawResult.columns[1].longValues![0];
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
    const constainColumn = isCached ? 'cached_tsq' : 'ts';

    const rawResult = await this.query(`
      select
        ${queryTsq} as tsq,
        ts,
        max(dur) as dur,
        utid,
        id
      from ${queryTable}
      where
        ${constainColumn} >= ${startNs - this.maxDurNs} and
        ${constainColumn} <= ${endNs}
      group by tsq
      order by tsq
    `);

    const numRows = slowlyCountRows(rawResult);
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
      slices.utids[row] = +cols[3].longValues![row];
      slices.ids[row] = +cols[4].longValues![row];
    }

    return slices;
  }

  async onDestroy() {
    await this.query(`drop table if exists ${this.tableName('sched_cached')}`);
  }
}

trackControllerRegistry.register(CpuSliceTrackController);
