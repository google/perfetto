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


// Allow to override via devtools for testing (note, needs to be done in the
// controller-thread).
(self as {} as {quantPx: number}).quantPx = 1;

class CpuFreqTrackController extends TrackController<Config, Data> {
  static readonly kind = CPU_FREQ_TRACK_KIND;
  private setup = false;
  private maxDurNs = 0;
  private maximumValueSeen = 0;

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const startNs = toNs(start);
    const endNs = toNs(end);

    const pxSize = (self as {} as {quantPx: number}).quantPx;

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
        this.maxDurNs = +maxDurFreqResult.columns![0].longValues![0];
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
            value as idle_value
          from experimental_counter_dur c
          where track_id = ${this.config.idleTrackId};
        `);

        const maxDurIdleResult =
            await this.query(`select max(dur) from ${this.tableName('idle')}`);
        if (maxDurIdleResult.numRecords === 1) {
          this.maxDurNs = Math.max(
              this.maxDurNs, +maxDurIdleResult.columns![0].longValues![0]);
        }

        await this.query(`create virtual table ${this.tableName('freq_idle')}
          using span_join(${this.tableName('freq')},
                          ${this.tableName('idle')});`);
      }

      this.setup = true;
    }

    const geqConstraint = this.config.idleTrackId === undefined ?
        `ts >= ${startNs - this.maxDurNs}` :
        `source_geq(ts, ${startNs - this.maxDurNs})`;
    const freqResult = await this.query(`
      select
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as tsq,
        ts,
        max(dur) as dur,
        case idle_value
          when 4294967295 then cast(-1 as double)
          else cast(idle_value as double)
        end as idle,
        freq_value as freq
      from ${this.tableName('freq_idle')}
      where ${geqConstraint} and ts <= ${endNs}
      group by tsq
    `);

    const numRows = +freqResult.numRecords;
    const data: Data = {
      start,
      end,
      resolution,
      length: numRows,
      maximumValue: this.maximumValue(),
      tsStarts: new Float64Array(numRows),
      tsEnds: new Float64Array(numRows),
      idles: new Int8Array(numRows),
      freqKHz: new Uint32Array(numRows),
    };

    const cols = freqResult.columns;
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

      data.tsStarts[row] = fromNs(startNsQ);
      data.tsEnds[row] = fromNs(endNsQ);
      data.idles[row] = +cols[3].doubleValues![row];
      data.freqKHz[row] = +cols[4].doubleValues![row];
    }

    return data;
  }

  private maximumValue() {
    return Math.max(this.config.maximumValue || 0, this.maximumValueSeen);
  }

}


trackControllerRegistry.register(CpuFreqTrackController);
