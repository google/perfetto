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
import {NUM, NUM_NULL, STR_NULL} from '../../common/query_result';
import {translateState} from '../../common/thread_state';
import {fromNs, toNs} from '../../common/time';
import {
  TrackController,
  trackControllerRegistry,
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
        io_wait as ioWait
      from thread_state
      where utid = ${this.config.utid} and utid != 0
    `);

    const queryRes = await this.query(`
      select ifnull(max(dur), 0) as maxDur
      from ${this.tableName('thread_state')}
    `);
    this.maxDurNs = queryRes.firstRow({maxDur: NUM}).maxDur;
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
        state = 'S' as is_sleep,
        max(dur) as dur,
        ifnull(cast(cpu as integer), -1) as cpu,
        state,
        ioWait,
        ifnull(id, -1) as id
      from ${this.tableName('thread_state')}
      where
        ts >= ${startNs - this.maxDurNs} and
        ts <= ${endNs}
      group by tsq, is_sleep
      order by tsq
    `;

    const queryRes = await this.query(query);
    const numRows = queryRes.numRows();

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

    const stringIndexes = new Map<
        {shortState: string | undefined; ioWait: boolean | undefined},
        number>();
    function internState(
        shortState: string|undefined, ioWait: boolean|undefined) {
      let idx = stringIndexes.get({shortState, ioWait});
      if (idx !== undefined) return idx;
      idx = data.strings.length;
      data.strings.push(translateState(shortState, ioWait));
      stringIndexes.set({shortState, ioWait}, idx);
      return idx;
    }
    const it = queryRes.iter({
      'tsq': NUM,
      'ts': NUM,
      'dur': NUM,
      'cpu': NUM,
      'state': STR_NULL,
      'ioWait': NUM_NULL,
      'id': NUM,
    });
    for (let row = 0; it.valid(); it.next(), row++) {
      const startNsQ = it.tsq;
      const startNs = it.ts;
      const durNs = it.dur;
      const endNs = startNs + durNs;

      let endNsQ = Math.floor((endNs + bucketNs / 2 - 1) / bucketNs) * bucketNs;
      endNsQ = Math.max(endNsQ, startNsQ + bucketNs);

      const cpu = it.cpu;
      const state = it.state || undefined;
      const ioWait = it.ioWait === null ? undefined : !!it.ioWait;
      const id = it.id;

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
    await this.query(`drop view if exists ${this.tableName('thread_state')}`);
  }
}

trackControllerRegistry.register(ThreadStateTrackController);
