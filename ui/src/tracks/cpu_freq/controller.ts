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
import {LIMIT} from '../../common/track_data';

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
  private maximumValueSeen = 0;

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const startNs = Math.round(start * 1e9);
    const endNs = Math.round(end * 1e9);

    if (!this.setup) {
      const result = await this.query(`
      select max(value) from
        counters where name = 'cpufreq'
        and ref = ${this.config.cpu}`);
      this.maximumValueSeen = +result.columns[0].doubleValues![0];

      await this.query(
        `create virtual table ${this.tableName('window')} using window;`);

      await this.query(`create view ${this.tableName('freq')}
          as select
            ts,
            lead(ts) over (order by ts) - ts as dur,
            ref as cpu,
            name as freq_name,
            value as freq_value
          from counters
          where name = 'cpufreq'
            and ref = ${this.config.cpu}
            and ref_type = 'cpu';
      `);

      await this.query(`create view ${this.tableName('idle')}
        as select
          ts,
          lead(ts) over (order by ts) - ts as dur,
          ref as cpu,
          name as idle_name,
          value as idle_value
        from counters
        where name = 'cpuidle'
          and ref = ${this.config.cpu}
          and ref_type = 'cpu';
      `);

      await this.query(`create virtual table ${this.tableName('freq_idle')}
              using span_join(${this.tableName('freq')} PARTITIONED cpu,
                              ${this.tableName('idle')} PARTITIONED cpu);`);

      await this.query(`create virtual table ${this.tableName('span_activity')}
      using span_join(${this.tableName('freq_idle')} PARTITIONED cpu,
                      ${this.tableName('window')});`);

      // TODO(taylori): Move the idle value processing to the TP.
      await this.query(`create view ${this.tableName('activity')}
      as select
        ts,
        dur,
        quantum_ts,
        cpu,
        case idle_value
          when 4294967295 then -1
          else idle_value
        end as idle,
        freq_value as freq
        from ${this.tableName('span_activity')};
      `);

      this.setup = true;
    }

    this.query(`update ${this.tableName('window')} set
    window_start = ${startNs},
    window_dur = ${Math.max(1, endNs - startNs)},
    quantum = 0`);

    const result = await this.engine.queryOneRow(`select count(*)
      from ${this.tableName('activity')}`);
    const isQuantized = result[0] > LIMIT;

    // Cast as double to avoid problem where values are sometimes
    // doubles, sometimes longs.
    let query = `select ts, dur, cast(idle as DOUBLE), freq
      from ${this.tableName('activity')} limit ${LIMIT}`;

    if (isQuantized) {
      // |resolution| is in s/px we want # ns for 10px window:
      const bucketSizeNs = Math.round(resolution * 10 * 1e9);
      const windowStartNs = Math.floor(startNs / bucketSizeNs) * bucketSizeNs;
      const windowDurNs = Math.max(1, endNs - windowStartNs);

      this.query(`update ${this.tableName('window')} set
      window_start = ${startNs},
      window_dur = ${windowDurNs},
      quantum = ${isQuantized ? bucketSizeNs : 0}`);

      query = `select
        min(ts) as ts,
        sum(dur) as dur,
        case
          when min(idle) = -1 then cast(-1 as DOUBLE)
          else cast(0 as DOUBLE)
        end as idle,
        sum(weighted_freq)/sum(dur) as freq_avg,
        quantum_ts
        from (
          select
          ts,
          dur,
          quantum_ts,
          freq*dur as weighted_freq,
          idle
          from ${this.tableName('activity')})
        group by quantum_ts limit ${LIMIT}`;
    }

    const freqResult = await this.query(query);

    const numRows = +freqResult.numRecords;
    const data: Data = {
      start,
      end,
      resolution,
      length: numRows,
      maximumValue: this.maximumValue(),
      isQuantized,
      tsStarts: new Float64Array(numRows),
      tsEnds: new Float64Array(numRows),
      idles: new Int8Array(numRows),
      freqKHz: new Uint32Array(numRows),
    };

    const cols = freqResult.columns;
    for (let row = 0; row < numRows; row++) {
      const startSec = fromNs(+cols[0].longValues![row]);
      data.tsStarts[row] = startSec;
      data.tsEnds[row] = startSec + fromNs(+cols[1].longValues![row]);
      data.idles[row] = +cols[2].doubleValues![row];
      data.freqKHz[row] = +cols[3].doubleValues![row];
    }

    return data;
  }

  private maximumValue() {
    return Math.max(this.config.maximumValue || 0, this.maximumValueSeen);
  }

}


trackControllerRegistry.register(CpuFreqTrackController);
