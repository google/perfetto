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
  private setup = false;
  private maxDurNs = 0;
  private maxTsEndNs = 0;
  private maximumValueSeen = 0;

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const startNs = toNs(start);
    const endNs = toNs(end);

    const pxSize = this.pxSize();

    // ns per quantization bucket (i.e. ns per pixel). /2 * 2 is to force it to
    // be an even number, so we can snap in the middle.
    const bucketNs = Math.round(resolution * 1e9 * pxSize / 2) * 2;

    if (!this.setup) {
      const result = await this.query(`
        select max(value)
        from counter
        where track_id = ${this.config.freqTrackId}`);
      this.maximumValueSeen = +result.columns[0].doubleValues![0];

      await this.query(`create view ${this.tableName('freq')} as
        select
          ts,
          dur,
          value as freq_value
        from experimental_counter_dur c
        where track_id = ${this.config.freqTrackId};
      `);

      const maxDurFreqResult =
          await this.query(`select max(dur) from ${this.tableName('freq')}`);
      if (maxDurFreqResult.numRecords === 1) {
        this.maxDurNs = maxDurFreqResult.columns[0].longValues![0];
      }

      if (this.config.idleTrackId === undefined) {
        await this.query(`create view ${this.tableName('freq_idle')} as
          select
            ts,
            dur,
            -1 as idle_value,
            freq_value
          from ${this.tableName('freq')};
        `);
      } else {
        await this.query(`create view ${this.tableName('idle')} as
          select
            ts,
            dur,
            iif(value = 4294967295, -1, value) as idle_value
          from experimental_counter_dur c
          where track_id = ${this.config.idleTrackId};
        `);

        const maxDurIdleResult =
            await this.query(`select max(dur) from ${this.tableName('idle')}`);
        if (maxDurIdleResult.numRecords === 1) {
          this.maxDurNs = Math.max(
              this.maxDurNs, maxDurIdleResult.columns[0].longValues![0]);
        }

        await this.query(`create virtual table ${this.tableName('freq_idle')}
          using span_join(${this.tableName('freq')},
                          ${this.tableName('idle')});`);
      }

      const maxTsResult = await this.query(
          `select max(ts), dur from ${this.tableName('freq_idle')}`);
      if (maxTsResult.numRecords === 1) {
        this.maxTsEndNs = maxTsResult.columns[0].longValues![0] +
            maxTsResult.columns[1].longValues![0];
      }

      this.setup = true;
    }

    const geqConstraint = this.config.idleTrackId === undefined ?
        `ts >= ${startNs - this.maxDurNs}` :
        `source_geq(ts, ${startNs} - ${this.maxDurNs})`;
    const freqResult = await this.query(`
      select
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as tsq,
        min(freq_value) as minFreq,
        max(freq_value) as maxFreq,
        value_at_max_ts(ts, freq_value) as lastFreq,
        value_at_max_ts(ts, idle_value) as lastIdleValue
      from ${this.tableName('freq_idle')}
      where ${geqConstraint} and ts <= ${endNs}
      group by tsq
    `);

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

  private maximumValue() {
    return Math.max(this.config.maximumValue || 0, this.maximumValueSeen);
  }

}


trackControllerRegistry.register(CpuFreqTrackController);
