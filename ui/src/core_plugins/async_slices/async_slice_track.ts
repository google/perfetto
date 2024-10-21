// Copyright (C) 2023 The Android Open Source Project
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

import {BigintMath as BIMath} from '../../base/bigint_math';
import {clamp} from '../../base/math_utils';
import {NAMED_ROW, NamedSliceTrack} from '../../frontend/named_slice_track';
import {SLICE_LAYOUT_FIT_CONTENT_DEFAULTS} from '../../frontend/slice_layout';
import {NewTrackArgs} from '../../frontend/track';
import {TrackEventDetails} from '../../public/selection';
import {Slice} from '../../public/track';
import {LONG_NULL} from '../../trace_processor/query_result';

export const THREAD_SLICE_ROW = {
  // Base columns (tsq, ts, dur, id, depth).
  ...NAMED_ROW,

  // Thread-specific columns.
  threadDur: LONG_NULL,
};
export type ThreadSliceRow = typeof THREAD_SLICE_ROW;

export class AsyncSliceTrack extends NamedSliceTrack<Slice, ThreadSliceRow> {
  constructor(
    args: NewTrackArgs,
    maxDepth: number,
    private readonly trackIds: number[],
  ) {
    super(args);
    this.sliceLayout = {
      ...SLICE_LAYOUT_FIT_CONTENT_DEFAULTS,
      depthGuess: maxDepth,
    };
  }

  getRowSpec(): ThreadSliceRow {
    return THREAD_SLICE_ROW;
  }

  rowToSlice(row: ThreadSliceRow): Slice {
    const namedSlice = this.rowToSliceBase(row);

    if (row.dur > 0n && row.threadDur !== null) {
      const fillRatio = clamp(BIMath.ratio(row.threadDur, row.dur), 0, 1);
      return {...namedSlice, fillRatio};
    } else {
      return namedSlice;
    }
  }

  getSqlSource(): string {
    // If we only have one track ID we can avoid the overhead of
    // experimental_slice_layout, and just go straight to the slice table.
    if (this.trackIds.length === 1) {
      return `
        select
          ts,
          dur,
          id,
          depth,
          ifnull(name, '[null]') as name,
          thread_dur as threadDur
        from slice
        where track_id = ${this.trackIds[0]}
      `;
    } else {
      return `
        select
          id,
          ts,
          dur,
          layout_depth as depth,
          ifnull(name, '[null]') as name,
          thread_dur as threadDur
        from experimental_slice_layout
        where filter_track_ids = '${this.trackIds.join(',')}'
      `;
    }
  }

  onUpdatedSlices(slices: Slice[]) {
    for (const slice of slices) {
      slice.isHighlighted = slice === this.hoveredSlice;
    }
  }

  async getSelectionDetails(
    id: number,
  ): Promise<TrackEventDetails | undefined> {
    const baseDetails = await super.getSelectionDetails(id);
    if (!baseDetails) return undefined;
    return {
      ...baseDetails,
      tableName: 'slice',
    };
  }
}
