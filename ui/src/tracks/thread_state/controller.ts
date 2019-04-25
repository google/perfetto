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
import {
  TrackController,
  trackControllerRegistry
} from '../../controller/track_controller';

import {
  Config,
  Data,
  THREAD_STATE_TRACK_KIND,
} from './common';

class ThreadStateTrackController extends TrackController<Config, Data> {
  static readonly kind = THREAD_STATE_TRACK_KIND;
  private busy = false;
  private setup = false;

  onBoundsChange(start: number, end: number, resolution: number): void {
    this.update(start, end, resolution);
  }

  private async update(start: number, end: number, resolution: number):
      Promise<void> {
    if (this.busy) return;
    this.busy = true;

    const startNs = Math.round(start * 1e9);
    const endNs = Math.round(end * 1e9);

    if (this.setup === false) {
      await this.query(`create view ${this.tableName('sched_wakeup')} AS
        select
          ts,
          lead(ts, 1, (select end_ts from trace_bounds))
            OVER(order by ts) - ts as dur,
          ref as utid
        from instants
        where name = 'sched_wakeup'
        and utid = ${this.config.utid}`);

      await this.query(
          `create virtual table ${this.tableName('window')} using window;`);

      // Get the first ts for this utid - whether a sched wakeup
      // or sched event.
      await this.query(`create view ${this.tableName('start')} as
      select min(ts) as ts from
        (select ts from ${this.tableName('sched_wakeup')} UNION
         select ts from sched where utid = ${this.config.utid})`);

      // Create an entry from first ts to either the first sched_wakeup
      // or to the end if there are no sched wakeups. This means
      // we will show all information we have even with no sched_wakeup events.
      // TODO(taylori): Once span outer join exists I should simplify this
      // by outer joining sched_wakeup and sched and then left joining with
      // window.
      await this.query(`create view ${this.tableName('fill')} AS
        select
        (select ts from ${this.tableName('start')}),
        (select coalesce(
          (select min(ts) from ${this.tableName('sched_wakeup')}),
          (select end_ts from trace_bounds)
        )) - (select ts from ${this.tableName('start')}) as dur,
        ${this.config.utid} as utid
        `);

      await this.query(`create view ${this.tableName('full_sched_wakeup')} as
        select * from ${this.tableName('sched_wakeup')} UNION
        select * from ${this.tableName('fill')}`);

      await this.query(`create virtual table ${this.tableName('span')}
        using span_left_join(
          ${this.tableName('full_sched_wakeup')} partitioned utid,
          sched partitioned utid)`);

      // Need to compute the lag(end_state) before joining with the window
      // table to avoid the first visible slice always having a null prev
      // end state.
      await this.query(`create view ${this.tableName('span_view')} as
        select ts, dur, utid,
        case
        when end_state is not null
        then 'Running'
        when lag(end_state) over ${this.tableName('ordered')} is not null
        then lag(end_state) over ${this.tableName('ordered')}
        else 'Runnable'
        end as state
        from ${this.tableName('span')}
        where utid = ${this.config.utid}
        window ${this.tableName('ordered')} as (order by ts)`);

      await this.query(`create virtual table ${this.tableName('current')}
        using span_join(
          ${this.tableName('window')},
          ${this.tableName('span_view')} partitioned utid)`);

      this.setup = true;
    }

    const windowDur = Math.max(1, endNs - startNs);

    this.query(`update ${this.tableName('window')} set
      window_start = ${startNs},
      window_dur = ${windowDur},
      quantum = 0`);

    const query = `select ts, cast(dur as double), utid,
      state from ${this.tableName('current')}`;

    const result = await this.query(query);

    const numRows = +result.numRecords;

    const summary: Data = {
      start,
      end,
      resolution,
      starts: new Float64Array(numRows),
      ends: new Float64Array(numRows),
      strings: [],
      state: new Uint16Array(numRows)
    };

    const stringIndexes = new Map<string, number>();
    function internString(str: string) {
      let idx = stringIndexes.get(str);
      if (idx !== undefined) return idx;
      idx = summary.strings.length;
      summary.strings.push(str);
      stringIndexes.set(str, idx);
      return idx;
    }

    for (let row = 0; row < numRows; row++) {
      const cols = result.columns;
      const start = fromNs(+cols[0].longValues![row]);
      summary.starts[row] = start;
      summary.ends[row] = start + fromNs(+cols[1].doubleValues![row]);
      summary.state[row] = internString(cols[3].stringValues![row]);
    }

    this.publish(summary);
    this.busy = false;
  }

  private async query(query: string) {
    const result = await this.engine.query(query);
    if (result.error) {
      console.error(`Query error "${query}": ${result.error}`);
      throw new Error(`Query error "${query}": ${result.error}`);
    }
    return result;
  }

  onDestroy(): void {
    if (this.setup) {
      this.query(`drop table ${this.tableName('window')}`);
      this.query(`drop table ${this.tableName('span')}`);
      this.query(`drop table ${this.tableName('current')}`);
      this.query(`drop view ${this.tableName('sched_wakeup')}`);
      this.query(`drop view ${this.tableName('fill')}`);
      this.query(`drop view ${this.tableName('full_sched_wakeup')}`);
      this.query(`drop view ${this.tableName('span_view')}`);
      this.setup = false;
    }
  }
}

trackControllerRegistry.register(ThreadStateTrackController);
