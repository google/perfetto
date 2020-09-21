// Copyright (C) 2020 The Android Open Source Project
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

import {assertTrue} from '../../base/logging';
import {Actions} from '../../common/actions';
import {fromNs, toNs} from '../../common/time';
import {globals} from '../../controller/globals';
import {
  TrackController,
  trackControllerRegistry,
} from '../../controller/track_controller';

import {Config, Data, DEBUG_SLICE_TRACK_KIND} from './common';

class DebugSliceTrackController extends TrackController<Config, Data> {
  static readonly kind = DEBUG_SLICE_TRACK_KIND;

  async onReload() {
    const rawResult = await this.query(`select max(depth) from debug_slices`);
    const maxDepth =
        (rawResult.numRecords === 0) ? 1 : rawResult.columns[0].longValues![0];
    globals.dispatch(
        Actions.updateTrackConfig({id: this.trackId, config: {maxDepth}}));
  }

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const rawResult = await this.query(
        `select id, name, ts, dur, depth from debug_slices where
        (ts + dur) >= ${toNs(start)} and ts <= ${toNs(end)}`);

    assertTrue(rawResult.columns.length === 5);
    const [idCol, nameCol, tsCol, durCol, depthCol] = rawResult.columns;
    const idValues = idCol.longValues! || idCol.doubleValues!;
    const tsValues = tsCol.longValues! || tsCol.doubleValues!;
    const durValues = durCol.longValues! || durCol.doubleValues!;

    const numRows = rawResult.numRecords;
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

    for (let i = 0; i < rawResult.numRecords; i++) {
      let sliceStart: number, sliceEnd: number;
      if (tsCol.isNulls![i] || durCol.isNulls![i]) {
        sliceStart = sliceEnd = -1;
      } else {
        sliceStart = tsValues[i];
        const sliceDur = durValues[i];
        sliceEnd = sliceStart + sliceDur;
      }
      slices.sliceIds[i] = idCol.isNulls![i] ? -1 : idValues[i];
      slices.starts[i] = fromNs(sliceStart);
      slices.ends[i] = fromNs(sliceEnd);
      slices.depths[i] = depthCol.isNulls![i] ? 0 : depthCol.longValues![i];
      const sliceName =
          nameCol.isNulls![i] ? '[null]' : nameCol.stringValues![i];
      slices.titles[i] = internString(sliceName);
      slices.isInstant[i] = 0;
    }

    return slices;
  }
}

trackControllerRegistry.register(DebugSliceTrackController);
