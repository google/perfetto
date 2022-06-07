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
import {NUM, NUM_NULL, QueryResult} from '../../common/query_result';
import {fromNs, toNs} from '../../common/time';
import {
  TrackController,
  trackControllerRegistry,
} from '../../controller/track_controller';

import {
  Config,
  CPU_FREQ_TRACK_KIND,
  Data,
} from './common';

class CpuFreqTrackController extends TrackController<Config, Data> {
  static readonly kind = CPU_FREQ_TRACK_KIND;

  private maxDurNs = 0;
  private maxTsEndNs = 0;
  private maximumValueSeen = 0;
  private cachedBucketNs = Number.MAX_SAFE_INTEGER;

  async onSetup() {
    await this.createFreqIdleViews();

    this.maximumValueSeen = await this.queryMaxFrequency();
    this.maxDurNs = await this.queryMaxSourceDur();

    const iter = (await this.query(`
      select max(ts) as maxTs, dur, count(1) as rowCount
      from ${this.tableName('freq_idle')}
    `)).firstRow({maxTs: NUM_NULL, dur: NUM_NULL, rowCount: NUM});
    if (iter.maxTs === null || iter.dur === null) {
      // We shoulnd't really hit this because trackDecider shouldn't create
      // the track in the first place if there are no entries. But could happen
      // if only one cpu has no cpufreq data.
      return;
    }
    this.maxTsEndNs = iter.maxTs + iter.dur;

    const rowCount = iter.rowCount;
    const bucketNs = this.cachedBucketSizeNs(rowCount);
    if (bucketNs === undefined) {
      return;
    }

    await this.query(`
      create table ${this.tableName('freq_idle_cached')} as
      select
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as cachedTsq,
        min(freqValue) as minFreq,
        max(freqValue) as maxFreq,
        value_at_max_ts(ts, freqValue) as lastFreq,
        value_at_max_ts(ts, idleValue) as lastIdleValue
      from ${this.tableName('freq_idle')}
      group by cachedTsq
      order by cachedTsq
    `);

    this.cachedBucketNs = bucketNs;
  }

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
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
    const freqResult = await this.queryData(startNs, endNs, bucketNs);
    assertTrue(freqResult.isComplete());

    const numRows = freqResult.numRows();
    const data: Data = {
      start,
      end,
      resolution,
      length: numRows,
      maximumValue: this.maximumValue(),
      maxTsEnd: this.maxTsEndNs,
      timestamps: new Float64Array(numRows),
      minFreqKHz: new Uint32Array(numRows),
      maxFreqKHz: new Uint32Array(numRows),
      lastFreqKHz: new Uint32Array(numRows),
      lastIdleValues: new Int8Array(numRows),
    };

    const it = freqResult.iter({
      'tsq': NUM,
      'minFreq': NUM,
      'maxFreq': NUM,
      'lastFreq': NUM,
      'lastIdleValue': NUM,
    });
    for (let i = 0; it.valid(); ++i, it.next()) {
      data.timestamps[i] = fromNs(it.tsq);
      data.minFreqKHz[i] = it.minFreq;
      data.maxFreqKHz[i] = it.maxFreq;
      data.lastFreqKHz[i] = it.lastFreq;
      data.lastIdleValues[i] = it.lastIdleValue;
    }

    return data;
  }

  private async queryData(startNs: number, endNs: number, bucketNs: number):
      Promise<QueryResult> {
    const isCached = this.cachedBucketNs <= bucketNs;

    if (isCached) {
      return this.query(`
        select
          cachedTsq / ${bucketNs} * ${bucketNs} as tsq,
          min(minFreq) as minFreq,
          max(maxFreq) as maxFreq,
          value_at_max_ts(cachedTsq, lastFreq) as lastFreq,
          value_at_max_ts(cachedTsq, lastIdleValue) as lastIdleValue
        from ${this.tableName('freq_idle_cached')}
        where
          cachedTsq >= ${startNs - this.maxDurNs} and
          cachedTsq <= ${endNs}
        group by tsq
        order by tsq
      `);
    }
    const minTsFreq = await this.query(`
      select ifnull(max(ts), 0) as minTs from ${this.tableName('freq')}
      where ts < ${startNs}
    `);

    let minTs = minTsFreq.iter({minTs: NUM}).minTs;
    if (this.config.idleTrackId !== undefined) {
      const minTsIdle = await this.query(`
        select ifnull(max(ts), 0) as minTs from ${this.tableName('idle')}
        where ts < ${startNs}
      `);
      minTs = Math.min(minTsIdle.iter({minTs: NUM}).minTs, minTs);
    }

    const geqConstraint = this.config.idleTrackId === undefined ?
        `ts >= ${minTs}` :
        `source_geq(ts, ${minTs})`;
    return this.query(`
      select
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as tsq,
        min(freqValue) as minFreq,
        max(freqValue) as maxFreq,
        value_at_max_ts(ts, freqValue) as lastFreq,
        value_at_max_ts(ts, idleValue) as lastIdleValue
      from ${this.tableName('freq_idle')}
      where
        ${geqConstraint} and
        ts <= ${endNs}
      group by tsq
      order by tsq
    `);
  }

  private async queryMaxFrequency(): Promise<number> {
    const result = await this.query(`
      select max(freqValue) as maxFreq
      from ${this.tableName('freq')}
    `);
    return result.firstRow({'maxFreq': NUM_NULL}).maxFreq || 0;
  }

  private async queryMaxSourceDur(): Promise<number> {
    const maxDurFreqResult = await this.query(
        `select ifnull(max(dur), 0) as maxDur from ${this.tableName('freq')}`);
    const maxDurNs = maxDurFreqResult.firstRow({'maxDur': NUM}).maxDur;
    if (this.config.idleTrackId === undefined) {
      return maxDurNs;
    }

    const maxDurIdleResult = await this.query(
        `select ifnull(max(dur), 0) as maxDur from ${this.tableName('idle')}`);
    return Math.max(maxDurNs, maxDurIdleResult.firstRow({maxDur: NUM}).maxDur);
  }

  private async createFreqIdleViews() {
    await this.query(`create view ${this.tableName('freq')} as
      select
        ts,
        dur,
        value as freqValue
      from experimental_counter_dur c
      where track_id = ${this.config.freqTrackId};
    `);

    if (this.config.idleTrackId === undefined) {
      await this.query(`create view ${this.tableName('freq_idle')} as
        select
          ts,
          dur,
          -1 as idleValue,
          freqValue
        from ${this.tableName('freq')};
      `);
      return;
    }

    await this.query(`
      create view ${this.tableName('idle')} as
      select
        ts,
        dur,
        iif(value = 4294967295, -1, cast(value as int)) as idleValue
      from experimental_counter_dur c
      where track_id = ${this.config.idleTrackId};
    `);

    await this.query(`
      create virtual table ${this.tableName('freq_idle')}
      using span_join(${this.tableName('freq')}, ${this.tableName('idle')});
    `);
  }

  private maximumValue() {
    return Math.max(this.config.maximumValue || 0, this.maximumValueSeen);
  }
}


trackControllerRegistry.register(CpuFreqTrackController);
