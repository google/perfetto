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

import {assertFalse} from '../../base/logging';
import {translateState} from '../../common/thread_state';
import {fromNs, toNs} from '../../common/time';
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

  private maxDurNs = 0;

  async onSetup() {
    await this.query(`
      create view ${this.tableName('thread_state')} as
      select
        id,
        ts,
        dur,
        cpu,
        state,
        io_wait
      from thread_state
      where utid = ${this.config.utid} and utid != 0
    `);

    const rawResult = await this.query(`
      select max(dur)
      from ${this.tableName('thread_state')}
    `);
    this.maxDurNs = rawResult.columns[0].longValues![0];
  }

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const resolutionNs = toNs(resolution);
    const startNs = toNs(start);
    const endNs = toNs(end);

    // ns per quantization bucket (i.e. ns per pixel). /2 * 2 is to force it to
    // be an even number, so we can snap in the middle.
    const bucketNs =
        Math.max(Math.round(resolutionNs * this.pxSize() / 2) * 2, 1);

    const query = `
      select
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as tsq,
        ts,
        max(dur) as dur,
        cpu,
        state,
        io_wait,
        id
      from ${this.tableName('thread_state')}
      where
        ts >= ${startNs - this.maxDurNs} and
        ts <= ${endNs}
      group by tsq, state, io_wait
      order by tsq, state, io_wait
    `;

    const result = await this.query(query);
    const numRows = +result.numRecords;

    const data: Data = {
      start,
      end,
      resolution,
      length: numRows,
      ids: new Float64Array(numRows),
      starts: new Float64Array(numRows),
      ends: new Float64Array(numRows),
      strings: [],
      state: new Uint16Array(numRows),
      cpu: new Int8Array(numRows),
    };

    const stringIndexes =
        new Map<{shortState: string, ioWait: boolean | undefined}, number>();
    function internState(shortState: string, ioWait: boolean|undefined) {
      let idx = stringIndexes.get({shortState, ioWait});
      if (idx !== undefined) return idx;
      idx = data.strings.length;
      data.strings.push(translateState(shortState, ioWait));
      stringIndexes.set({shortState, ioWait}, idx);
      return idx;
    }

    for (let row = 0; row < numRows; row++) {
      const cols = result.columns;
      const startNsQ = +cols[0].longValues![row];
      const startNs = +cols[1].longValues![row];
      const durNs = +cols[2].longValues![row];
      const endNs = startNs + durNs;

      let endNsQ = Math.floor((endNs + bucketNs / 2 - 1) / bucketNs) * bucketNs;
      endNsQ = Math.max(endNsQ, startNsQ + bucketNs);

      const cpu = cols[3].isNulls![row] ? -1 : cols[3].longValues![row];
      const state = cols[4].stringValues![row];
      const ioWait =
          cols[5].isNulls![row] ? undefined : !!cols[5].longValues![row];
      const id = cols[6].isNulls![row] ? -1 : cols[6].longValues![row];

      // We should never have the end timestamp being the same as the bucket
      // start.
      assertFalse(startNsQ === endNsQ);

      data.starts[row] = fromNs(startNsQ);
      data.ends[row] = fromNs(endNsQ);
      data.state[row] = internState(state, ioWait);
      data.ids[row] = id;
      data.cpu[row] = cpu;
    }
    return data;
  }

  async onDestroy() {
    await this.query(`drop table if exists ${this.tableName('thread_state')}`);
  }
}

trackControllerRegistry.register(ThreadStateTrackController);
