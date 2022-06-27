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

import {NUM, NUM_NULL, STR} from '../../common/query_result';
import {fromNs, toNs} from '../../common/time';
import {
  TrackController,
  trackControllerRegistry,
} from '../../controller/track_controller';

import {Config, Data, SLICE_TRACK_KIND} from './common';

// the lowest bucketNs gets is 2, but add some room in case of fp error
const MIN_QUANT_NS = 3;

export class ChromeSliceTrackController extends TrackController<Config, Data> {
  static kind = SLICE_TRACK_KIND;
  private maxDurNs = 0;

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const startNs = toNs(start);
    const endNs = toNs(end);

    const pxSize = this.pxSize();

    // ns per quantization bucket (i.e. ns per pixel). /2 * 2 is to force it to
    // be an even number, so we can snap in the middle.
    const bucketNs = Math.max(Math.round(resolution * 1e9 * pxSize / 2) * 2, 1);

    const isThreadSlice = this.config.isThreadSlice;
    let tableName = this.namespaceTable('slice');
    let threadDurQuery = ', dur';
    if (isThreadSlice) {
      tableName = this.namespaceTable('thread_slice');
      threadDurQuery = ', iif(thread_dur IS NULL, dur, thread_dur)';
    }

    if (this.maxDurNs === 0) {
      const query = `
          SELECT max(iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts, dur))
          AS maxDur FROM ${tableName} WHERE track_id = ${this.config.trackId}`;
      const queryRes = await this.query(query);
      this.maxDurNs = queryRes.firstRow({maxDur: NUM_NULL}).maxDur || 0;
    }

    // Buckets are always even and positive, don't quantize once we zoom to
    // nanosecond-scale, so that we can see exact sizes.
    let tsq = `ts`;
    if (bucketNs > MIN_QUANT_NS) {
      tsq = `(ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs}`;
    }

    const query = `
      SELECT
        ${tsq} as tsq,
        ts,
        max(iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts, dur)) as dur,
        depth,
        id as sliceId,
        ifnull(name, '[null]') as name,
        dur = 0 as isInstant,
        dur = -1 as isIncomplete
        ${threadDurQuery} as threadDur
      FROM ${tableName}
      WHERE track_id = ${this.config.trackId} AND
        ts >= (${startNs - this.maxDurNs}) AND
        ts <= ${endNs}
      GROUP BY depth, tsq`;
    const queryRes = await this.query(query);

    const numRows = queryRes.numRows();
    const slices: Data = {
      start,
      end,
      resolution,
      length: numRows,
      strings: [],
      sliceIds: new Float64Array(numRows),
      starts: new Float64Array(numRows),
      ends: new Float64Array(numRows),
      depths: new Uint16Array(numRows),
      titles: new Uint16Array(numRows),
      isInstant: new Uint16Array(numRows),
      isIncomplete: new Uint16Array(numRows),
      cpuTimeRatio: new Float64Array(numRows),
    };

    const stringIndexes = new Map<string, number>();
    function internString(str: string) {
      let idx = stringIndexes.get(str);
      if (idx !== undefined) return idx;
      idx = slices.strings.length;
      slices.strings.push(str);
      stringIndexes.set(str, idx);
      return idx;
    }

    const it = queryRes.iter({
      tsq: NUM,
      ts: NUM,
      dur: NUM,
      depth: NUM,
      sliceId: NUM,
      name: STR,
      isInstant: NUM,
      isIncomplete: NUM,
      threadDur: NUM,
    });
    for (let row = 0; it.valid(); it.next(), row++) {
      const startNsQ = it.tsq;
      const startNs = it.ts;
      const durNs = it.dur;
      const endNs = startNs + durNs;

      let endNsQ = endNs;
      if (bucketNs > MIN_QUANT_NS) {
        endNsQ = Math.floor((endNs + bucketNs / 2 - 1) / bucketNs) * bucketNs;
        endNsQ = Math.max(endNsQ, startNsQ + bucketNs);
      }

      let isInstant = it.isInstant;
      // Floating point rounding with large numbers of nanoseconds can mean
      // there isn't enough precision to distinguish the start and end of a very
      // short event so we just display the event as an instant when zoomed in
      // rather than fail completely if the start and end time are the same.
      if (startNsQ === endNsQ) {
        isInstant = 1;
      }

      slices.starts[row] = fromNs(startNsQ);
      slices.ends[row] = fromNs(endNsQ);
      slices.depths[row] = it.depth;
      slices.sliceIds[row] = it.sliceId;
      slices.titles[row] = internString(it.name);
      slices.isInstant[row] = isInstant;
      slices.isIncomplete[row] = it.isIncomplete;

      let cpuTimeRatio = 1;
      if (!isInstant && !it.isIncomplete) {
        // Rounding the CPU time ratio to two decimal places and ensuring
        // it is less than or equal to one, incase the thread duration exceeds
        // the total duration.
        cpuTimeRatio =
            Math.min(Math.round((it.threadDur / it.dur) * 100) / 100, 1);
      }
      slices.cpuTimeRatio![row] = cpuTimeRatio;
    }
    return slices;
  }
}


trackControllerRegistry.register(ChromeSliceTrackController);
