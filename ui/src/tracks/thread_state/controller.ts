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
import {LIMIT} from '../../common/track_data';

import {
  TrackController,
  trackControllerRegistry
} from '../../controller/track_controller';

import {
  Config,
  Data,
  groupBusyStates,
  THREAD_STATE_TRACK_KIND,
} from './common';

class ThreadStateTrackController extends TrackController<Config, Data> {
  static readonly kind = THREAD_STATE_TRACK_KIND;
  private setup = false;

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const startNs = toNs(start);
    const endNs = toNs(end);
    let minNs = 0;
    if (groupBusyStates(resolution)) {
      // Ns for 1px (the smallest state to display)
      minNs = Math.round(resolution * 1e9);
    }

    if (this.setup === false) {
      await this.query(
          `create virtual table ${this.tableName('window')} using window;`);

      await this.query(`create view ${this.tableName('long_states')} as
      select * from thread_state where dur >= ${minNs} and utid = ${
          this.config.utid}`);

      // Create a slice from the first ts to the end of the trace. To
      // be span joined with the long states - This effectively combines all
      // of the short states into a single 'Busy' state.
      await this.query(`create view ${this.tableName('fill_gaps')} as select
      (select min(ts) from thread_state where utid = ${this.config.utid}) as ts,
      (select end_ts from trace_bounds) -
      (select min(ts) from thread_state where utid = ${
          this.config.utid}) as dur,
      ${this.config.utid} as utid`);

      await this.query(`create virtual table ${this.tableName('summarized')}
      using span_left_join(${this.tableName('fill_gaps')} partitioned utid,
      ${this.tableName('long_states')} partitioned utid)`);

      await this.query(`create virtual table ${this.tableName('current')}
      using span_join(
        ${this.tableName('window')},
        ${this.tableName('summarized')} partitioned utid)`);

      this.setup = true;
    }

    const windowDurNs = Math.max(1, endNs - startNs);

    this.query(`update ${this.tableName('window')} set
     window_start=${startNs},
     window_dur=${windowDurNs},
     quantum=0`);

    this.query(`drop view if exists ${this.tableName('long_states')}`);
    this.query(`drop view if exists ${this.tableName('fill_gaps')}`);

    await this.query(`create view ${this.tableName('long_states')} as
      select * from thread_state where dur >= ${minNs} and utid = ${
        this.config.utid}`);

    await this.query(`create view ${this.tableName('fill_gaps')} as select
      (select min(ts) from thread_state where utid = ${this.config.utid}) as ts,
      (select end_ts from trace_bounds) -
      (select min(ts) from thread_state where utid = ${
        this.config.utid}) as dur,
      ${this.config.utid} as utid`);

    const query = `select ts, cast(dur as double), utid,
    case when state is not null then state else 'Busy' end as state,
    cast(cpu as double)
    from ${this.tableName('current')} limit ${LIMIT}`;

    const result = await this.query(query);

    const numRows = +result.numRecords;

    const summary: Data = {
      start,
      end,
      resolution,
      length: numRows,
      starts: new Float64Array(numRows),
      ends: new Float64Array(numRows),
      strings: [],
      state: new Uint16Array(numRows),
      cpu: new Uint8Array(numRows)
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
      summary.cpu[row] = +cols[4].doubleValues![row];
    }

    return summary;
  }

  onDestroy(): void {
    if (this.setup) {
      this.query(`drop table ${this.tableName('window')}`);
      this.query(`drop table ${this.tableName('current')}`);
      this.query(`drop table ${this.tableName('summarized')}`);
      this.query(`drop view ${this.tableName('long_states')}`);
      this.query(`drop view ${this.tableName('fill_gaps')}`);
      this.setup = false;
    }
  }
}

trackControllerRegistry.register(ThreadStateTrackController);
