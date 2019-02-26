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

import {fromNs} from '../../common/time';
import {
  TrackController,
  trackControllerRegistry
} from '../../controller/track_controller';

import {
  Config,
  COUNTER_TRACK_KIND,
  Data,
} from './common';

class CounterTrackController extends TrackController<Config, Data> {
  static readonly kind = COUNTER_TRACK_KIND;
  private busy = false;
  private setup = false;
  private maximumValueSeen = 0;
  private minimumValueSeen = 0;

  onBoundsChange(start: number, end: number, resolution: number): void {
    this.update(start, end, resolution);
  }

  private async update(start: number, end: number, resolution: number):
      Promise<void> {
    // TODO: we should really call TraceProcessor.Interrupt() at this point.
    if (this.busy) return;

    const startNs = Math.round(start * 1e9);
    const endNs = Math.round(end * 1e9);

    this.busy = true;
    if (!this.setup) {
      const result = await this.query(`
      select max(value), min(value) from
        counters where name = '${this.config.name}'
        and ref = ${this.config.ref}`);
      this.maximumValueSeen = +result.columns[0].doubleValues![0];
      this.minimumValueSeen = +result.columns[1].doubleValues![0];
      await this.query(
        `create virtual table ${this.tableName('window')} using window;`);

      // TODO(taylori): Remove this view once span_join is fixed.
      await this.query(`create view ${this.tableName('window_cpu')} as
        select ts, dur from ${this.tableName('window')} where cpu = 0;`);

      await this.query(`create view ${this.tableName('counter_view')} as
        select ts,
        lead(ts) over (partition by ref_type order by ts) - ts as dur,
        value, name, ref
        from counters
        where name = '${this.config.name}' and ref = ${this.config.ref};`);

      await this.query(`create virtual table ${this.tableName('span')} using
        span_join(${this.tableName('counter_view')},
        ${this.tableName('window')});`);
      this.setup = true;
    }

    const isQuantized = this.shouldSummarize(resolution);
    // |resolution| is in s/px we want # ns for 10px window:
    const bucketSizeNs = Math.round(resolution * 10 * 1e9);
    let windowStartNs = startNs;
    if (isQuantized) {
      windowStartNs = Math.floor(windowStartNs / bucketSizeNs) * bucketSizeNs;
    }
    const windowDurNs = Math.max(1, endNs - windowStartNs);

    this.query(`update ${this.tableName('window')} set
    window_start=${windowStartNs},
    window_dur=${windowDurNs},
    quantum=${isQuantized ? bucketSizeNs : 0}`);
      
    let query = `select min(ts) as ts,
      sum(weighted_value)/sum(dur) as value
      from (select
        ts,
        dur,
        quantum_ts,
        value*dur as weighted_value
        from ${this.tableName('span')})
      group by quantum_ts;`;

    if (!isQuantized) {
      // TODO(hjd): Implement window clipping.
      query = `select ts, value
        from (select
          ts,
          lead(ts) over (partition by ref_type order by ts) as ts_end,
          value
          from counters
          where name = '${this.config.name}' and ref = ${this.config.ref})
      where ts <= ${endNs} and ${startNs} <= ts_end;`;
    }

    const rawResult = await this.query(query);

    const numRows = +rawResult.numRecords;

    const data: Data = {
      start,
      end,
      isQuantized,
      maximumValue: this.maximumValue(),
      minimumValue: this.minimumValue(),
      resolution,
      timestamps: new Float64Array(numRows),
      values: new Float64Array(numRows),
    };

    const cols = rawResult.columns;
    for (let row = 0; row < numRows; row++) {
      const startSec = fromNs(+cols[0].longValues![row]);
      const value = +cols[1].doubleValues![row];
      data.timestamps[row] = startSec;
      data.values[row] = value;
    }

    this.publish(data);
    this.busy = false;
  }

  private maximumValue() {
    return Math.max(this.config.maximumValue || 0, this.maximumValueSeen);
  }

  private minimumValue() {
    return Math.min(this.config.minimumValue || 0, this.minimumValueSeen);
  }

  private async query(query: string) {
    const result = await this.engine.query(query);
    if (result.error) {
      console.error(`Query error "${query}": ${result.error}`);
      throw new Error(`Query error "${query}": ${result.error}`);
    }
    return result;
  }
}

trackControllerRegistry.register(CounterTrackController);
