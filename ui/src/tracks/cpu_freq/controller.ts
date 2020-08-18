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
import {RawQueryResult} from '../../common/protos';
import {iter, NUM, slowlyCountRows} from '../../common/query_iterator';
import {fromNs, toNs} from '../../common/time';
import {
  TrackController,
  trackControllerRegistry
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

    const result = await this.query(`
      select max(ts), dur, count(1)
      from ${this.tableName('freq_idle')}
    `);
    this.maxTsEndNs =
        result.columns[0].longValues![0] + result.columns[1].longValues![0];

    const rowCount = result.columns[2].longValues![0];
    const bucketNs = this.cachedBucketSizeNs(rowCount);
    if (bucketNs === undefined) {
      return;
    }
    await this.query(`
      create table ${this.tableName('freq_idle_cached')} as
      select
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as cached_tsq,
        min(freq_value) as min_freq,
        max(freq_value) as max_freq,
        value_at_max_ts(ts, freq_value) as last_freq,
        value_at_max_ts(ts, idle_value) as last_idle_value
      from ${this.tableName('freq_idle')}
      group by cached_tsq
      order by cached_tsq
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

    const numRows = slowlyCountRows(freqResult);
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

    const it = iter(
        {
          'tsq': NUM,
          'minFreq': NUM,
          'maxFreq': NUM,
          'lastFreq': NUM,
          'lastIdleValue': NUM,
        },
        freqResult);
    for (let i = 0; it.valid(); ++i, it.next()) {
      data.timestamps[i] = fromNs(it.row.tsq);
      data.minFreqKHz[i] = it.row.minFreq;
      data.maxFreqKHz[i] = it.row.maxFreq;
      data.lastFreqKHz[i] = it.row.lastFreq;
      data.lastIdleValues[i] = it.row.lastIdleValue;
    }
    return data;
  }

  private async queryData(startNs: number, endNs: number, bucketNs: number):
      Promise<RawQueryResult> {
    const isCached = this.cachedBucketNs <= bucketNs;

    if (isCached) {
      return this.query(`
        select
          cached_tsq / ${bucketNs} * ${bucketNs} as tsq,
          min(min_freq) as minFreq,
          max(max_freq) as maxFreq,
          value_at_max_ts(cached_tsq, last_freq) as lastFreq,
          value_at_max_ts(cached_tsq, last_idle_value) as lastIdleValue
        from ${this.tableName('freq_idle_cached')}
        where
          cached_tsq >= ${startNs - this.maxDurNs} and
          cached_tsq <= ${endNs}
        group by tsq
        order by tsq
      `);
    }

    const minTsFreq = await this.query(`
      select ifnull(max(ts), 0) from ${this.tableName('freq')}
      where ts < ${startNs}
    `);
    let minTs = minTsFreq.columns[0].longValues![0];
    if (this.config.idleTrackId !== undefined) {
      const minTsIdle = await this.query(`
        select ifnull(max(ts), 0) from ${this.tableName('idle')}
        where ts < ${startNs}
      `);
      minTs = Math.min(minTsIdle.columns[0].longValues![0], minTs);
    }
    const geqConstraint = this.config.idleTrackId === undefined ?
        `ts >= ${minTs}` :
        `source_geq(ts, ${minTs})`;
    return this.query(`
      select
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as tsq,
        min(freq_value) as minFreq,
        max(freq_value) as maxFreq,
        value_at_max_ts(ts, freq_value) as lastFreq,
        value_at_max_ts(ts, idle_value) as lastIdleValue
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
      select max(freq_value)
      from ${this.tableName('freq')}
    `);
    return result.columns[0].doubleValues![0];
  }

  private async queryMaxSourceDur(): Promise<number> {
    const maxDurFreqResult =
        await this.query(`select max(dur) from ${this.tableName('freq')}`);
    const maxFreqDurNs = maxDurFreqResult.columns[0].longValues![0];
    if (this.config.idleTrackId === undefined) {
      return maxFreqDurNs;
    }

    const maxDurIdleResult =
        await this.query(`select max(dur) from ${this.tableName('idle')}`);
    return Math.max(maxFreqDurNs, maxDurIdleResult.columns[0].longValues![0]);
  }

  private async createFreqIdleViews() {
    await this.query(`create view ${this.tableName('freq')} as
      select
        ts,
        dur,
        value as freq_value
      from experimental_counter_dur c
      where track_id = ${this.config.freqTrackId};
    `);

    if (this.config.idleTrackId === undefined) {
      await this.query(`create view ${this.tableName('freq_idle')} as
        select
          ts,
          dur,
          -1 as idle_value,
          freq_value
        from ${this.tableName('freq')};
      `);
      return;
    }

    await this.query(`
      create view ${this.tableName('idle')} as
      select
        ts,
        dur,
        iif(value = 4294967295, -1, cast(value as int)) as idle_value
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
