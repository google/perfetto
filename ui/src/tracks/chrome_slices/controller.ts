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

import {slowlyCountRows} from '../../common/query_iterator';
import {fromNs, toNs} from '../../common/time';
import {
  TrackController,
  trackControllerRegistry,
} from '../../controller/track_controller';

import {Config, Data, SLICE_TRACK_KIND} from './common';


class ChromeSliceTrackController extends TrackController<Config, Data> {
  static readonly kind = SLICE_TRACK_KIND;
  private maxDurNs = 0;

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const startNs = toNs(start);
    const endNs = toNs(end);

    const pxSize = this.pxSize();

    // ns per quantization bucket (i.e. ns per pixel). /2 * 2 is to force it to
    // be an even number, so we can snap in the middle.
    const bucketNs = Math.max(Math.round(resolution * 1e9 * pxSize / 2) * 2, 1);
    const tableName = this.namespaceTable('slice');

    if (this.maxDurNs === 0) {
      const query = `SELECT max(dur) FROM ${tableName} WHERE track_id = ${
          this.config.trackId}`;
      const rawResult = await this.query(query);
      if (slowlyCountRows(rawResult) === 1) {
        this.maxDurNs = rawResult.columns[0].longValues![0];
      }
    }

    const query = `
      SELECT
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as tsq,
        ts,
        max(dur),
        depth,
        id as slice_id,
        name,
        dur = 0 as is_instant,
        dur = -1 as is_incomplete
      FROM ${tableName}
      WHERE track_id = ${this.config.trackId} AND
        ts >= (${startNs - this.maxDurNs}) AND
        ts <= ${endNs}
      GROUP BY depth, tsq`;
    const rawResult = await this.query(query);

    const numRows = slowlyCountRows(rawResult);
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

    const cols = rawResult.columns;
    for (let row = 0; row < numRows; row++) {
      const startNsQ = +cols[0].longValues![row];
      const startNs = +cols[1].longValues![row];
      const durNs = +cols[2].longValues![row];
      const endNs = startNs + durNs;
      const isInstant = +cols[6].longValues![row];
      const isIncomplete = +cols[7].longValues![row];

      let endNsQ = Math.floor((endNs + bucketNs / 2 - 1) / bucketNs) * bucketNs;
      endNsQ = Math.max(endNsQ, startNsQ + bucketNs);

      if (!isInstant && startNsQ === endNsQ) {
        throw new Error(
            'Expected startNsQ and endNsQ to differ (' +
            `startNsQ: ${startNsQ}, startNs: ${startNs},` +
            ` endNsQ: ${endNsQ}, durNs: ${durNs},` +
            ` endNs: ${endNs}, bucketNs: ${bucketNs})`);
      }

      slices.starts[row] = fromNs(startNsQ);
      slices.ends[row] = fromNs(endNsQ);
      slices.depths[row] = +cols[3].longValues![row];
      slices.sliceIds[row] = +cols[4].longValues![row];
      slices.titles[row] = internString(cols[5].stringValues![row]);
      slices.isInstant[row] = isInstant;
      slices.isIncomplete[row] = isIncomplete;
    }
    return slices;
  }
}


trackControllerRegistry.register(ChromeSliceTrackController);
