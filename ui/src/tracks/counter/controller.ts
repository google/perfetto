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

import {NUM, NUM_NULL} from '../../common/query_result';
import {fromNs, toNs} from '../../common/time';
import {
  TrackController,
  trackControllerRegistry,
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
  private maximumDeltaSeen = 0;
  private minimumDeltaSeen = 0;
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
            value,
            delta
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
            lead(value, 1, value) over (order by ts) - value as delta,
            value
          from ${this.namespaceTable('counter')}
          where track_id = ${this.config.trackId};
        `);
      }

      const maxDurResult = await this.query(`
          select
            max(
              iif(dur != -1, dur, (select end_ts from trace_bounds) - ts)
            ) as maxDur
          from ${this.tableName('counter_view')}
      `);
      this.maxDurNs = maxDurResult.firstRow({maxDur: NUM_NULL}).maxDur || 0;

      const queryRes = await this.query(`
        select
          ifnull(max(value), 0) as maxValue,
          ifnull(min(value), 0) as minValue,
          ifnull(max(delta), 0) as maxDelta,
          ifnull(min(delta), 0) as minDelta
        from ${this.tableName('counter_view')}`);
      const row = queryRes.firstRow(
          {maxValue: NUM, minValue: NUM, maxDelta: NUM, minDelta: NUM});
      this.maximumValueSeen = row.maxValue;
      this.minimumValueSeen = row.minValue;
      this.maximumDeltaSeen = row.maxDelta;
      this.minimumDeltaSeen = row.minDelta;

      this.setup = true;
    }

    const queryRes = await this.query(`
      select
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as tsq,
        min(value) as minValue,
        max(value) as maxValue,
        sum(delta) as totalDelta,
        value_at_max_ts(ts, id) as lastId,
        value_at_max_ts(ts, value) as lastValue
      from ${this.tableName('counter_view')}
      where ts >= ${startNs - this.maxDurNs} and ts <= ${endNs}
      group by tsq
      order by tsq
    `);

    const numRows = queryRes.numRows();

    const data: Data = {
      start,
      end,
      length: numRows,
      maximumValue: this.maximumValue(),
      minimumValue: this.minimumValue(),
      maximumDelta: this.maximumDeltaSeen,
      minimumDelta: this.minimumDeltaSeen,
      resolution,
      timestamps: new Float64Array(numRows),
      lastIds: new Float64Array(numRows),
      minValues: new Float64Array(numRows),
      maxValues: new Float64Array(numRows),
      lastValues: new Float64Array(numRows),
      totalDeltas: new Float64Array(numRows),
    };

    const it = queryRes.iter({
      'tsq': NUM,
      'lastId': NUM,
      'minValue': NUM,
      'maxValue': NUM,
      'lastValue': NUM,
      'totalDelta': NUM,
    });
    for (let row = 0; it.valid(); it.next(), row++) {
      data.timestamps[row] = fromNs(it.tsq);
      data.lastIds[row] = it.lastId;
      data.minValues[row] = it.minValue;
      data.maxValues[row] = it.maxValue;
      data.lastValues[row] = it.lastValue;
      data.totalDeltas[row] = it.totalDelta;
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
