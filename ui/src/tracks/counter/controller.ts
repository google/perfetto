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

import {iter, NUM, slowlyCountRows} from '../../common/query_iterator';
import {fromNs, toNs} from '../../common/time';
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
  private setup = false;
  private maximumValueSeen = 0;
  private minimumValueSeen = 0;
  private maxDurNs = 0;

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const startNs = toNs(start);
    const endNs = toNs(end);

    const pxSize = this.pxSize();

    // ns per quantization bucket (i.e. ns per pixel). /2 * 2 is to force it to
    // be an even number, so we can snap in the middle.
    const bucketNs = Math.max(Math.round(resolution * 1e9 * pxSize / 2) * 2, 1);

    if (!this.setup) {
      if (this.config.namespace === undefined) {
        await this.query(`
          create view ${this.tableName('counter_view')} as
          select
            id,
            ts,
            dur,
            value
          from experimental_counter_dur
          where track_id = ${this.config.trackId};
        `);
      } else {
        await this.query(`
          create view ${this.tableName('counter_view')} as
          select
            id,
            ts,
            lead(ts, 1, ts) over (order by ts) - ts as dur,
            value
          from ${this.namespaceTable('counter')}
          where track_id = ${this.config.trackId};
        `);
      }

      const maxDurResult = await this.query(`
          select
            max(
              iif(dur != -1, dur, (select end_ts from trace_bounds) - ts)
            )
          from ${this.tableName('counter_view')}
      `);
      if (slowlyCountRows(maxDurResult) === 1) {
        this.maxDurNs = maxDurResult.columns[0].longValues![0];
      }

      const result = await this.query(`
        select max(value), min(value)
        from ${this.tableName('counter_view')}`);
      this.maximumValueSeen = +result.columns[0].doubleValues![0];
      this.minimumValueSeen = +result.columns[1].doubleValues![0];

      this.setup = true;
    }

    const rawResult = await this.query(`
      select
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as tsq,
        min(value) as minValue,
        max(value) as maxValue,
        value_at_max_ts(ts, id) as lastId,
        value_at_max_ts(ts, value) as lastValue
      from ${this.tableName('counter_view')}
      where ts >= ${startNs - this.maxDurNs} and ts <= ${endNs}
      group by tsq
      order by tsq
    `);

    const numRows = slowlyCountRows(rawResult);

    const data: Data = {
      start,
      end,
      length: numRows,
      maximumValue: this.maximumValue(),
      minimumValue: this.minimumValue(),
      resolution,
      timestamps: new Float64Array(numRows),
      lastIds: new Float64Array(numRows),
      minValues: new Float64Array(numRows),
      maxValues: new Float64Array(numRows),
      lastValues: new Float64Array(numRows),
    };

    const it = iter(
        {
          'tsq': NUM,
          'lastId': NUM,
          'minValue': NUM,
          'maxValue': NUM,
          'lastValue': NUM
        },
        rawResult);
    for (let i = 0; it.valid(); ++i, it.next()) {
      data.timestamps[i] = fromNs(it.row.tsq);
      data.lastIds[i] = it.row.lastId;
      data.minValues[i] = it.row.minValue;
      data.maxValues[i] = it.row.maxValue;
      data.lastValues[i] = it.row.lastValue;
    }

    return data;
  }

  private maximumValue() {
    if (this.config.maximumValue === undefined) {
      return this.maximumValueSeen;
    } else {
      return this.config.maximumValue;
    }
  }

  private minimumValue() {
    if (this.config.minimumValue === undefined) {
      return this.minimumValueSeen;
    } else {
      return this.config.minimumValue;
    }
  }
}

trackControllerRegistry.register(CounterTrackController);
