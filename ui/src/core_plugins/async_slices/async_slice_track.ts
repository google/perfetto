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

import {
  NAMED_ROW,
  NamedRow,
  NamedSliceTrack,
} from '../../frontend/named_slice_track';
import {SLICE_LAYOUT_FIT_CONTENT_DEFAULTS} from '../../frontend/slice_layout';
import {NewTrackArgs} from '../../frontend/track';
import {TrackEventDetails} from '../../public/selection';
import {Slice} from '../../public/track';

export class AsyncSliceTrack extends NamedSliceTrack {
  constructor(
    args: NewTrackArgs,
    maxDepth: number,
    private trackIds: number[],
  ) {
    super(args);
    this.sliceLayout = {
      ...SLICE_LAYOUT_FIT_CONTENT_DEFAULTS,
      depthGuess: maxDepth,
    };
  }

  getRowSpec(): NamedRow {
    return NAMED_ROW;
  }

  rowToSlice(row: NamedRow): Slice {
    return this.rowToSliceBase(row);
  }

  getSqlSource(): string {
    return `
      select
        ts,
        dur,
        layout_depth as depth,
        ifnull(name, '[null]') as name,
        id,
        thread_dur as threadDur
      from experimental_slice_layout
      where filter_track_ids = '${this.trackIds.join(',')}'
    `;
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
