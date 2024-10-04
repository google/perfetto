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

import {colorForState} from '../../core/colorizer';
import {
  BASE_ROW,
  BaseSliceTrack,
  OnSliceClickArgs,
} from '../../frontend/base_slice_track';
import {
  SLICE_LAYOUT_FLAT_DEFAULTS,
  SliceLayout,
} from '../../frontend/slice_layout';
import {NewTrackArgs} from '../../frontend/track';
import {NUM_NULL, STR} from '../../trace_processor/query_result';
import {Slice} from '../../public/track';
import {translateState} from '../../trace_processor/sql_utils/thread_state';
import {TrackEventDetails} from '../../public/selection';

export const THREAD_STATE_ROW = {
  ...BASE_ROW,
  state: STR,
  ioWait: NUM_NULL,
};

export type ThreadStateRow = typeof THREAD_STATE_ROW;

export class ThreadStateTrack extends BaseSliceTrack<Slice, ThreadStateRow> {
  protected sliceLayout: SliceLayout = {...SLICE_LAYOUT_FLAT_DEFAULTS};

  constructor(
    args: NewTrackArgs,
    private utid: number,
  ) {
    super(args);
  }

  // This is used by the base class to call iter().
  getRowSpec(): ThreadStateRow {
    return THREAD_STATE_ROW;
  }

  getSqlSource(): string {
    // Do not display states: 'S' (sleeping), 'I' (idle kernel thread).
    return `
      select
        id,
        ts,
        dur,
        cpu,
        state,
        io_wait as ioWait,
        0 as depth
      from thread_state
      where
        utid = ${this.utid} and
        state not in ('S', 'I')
    `;
  }

  rowToSlice(row: ThreadStateRow): Slice {
    const baseSlice = this.rowToSliceBase(row);
    const ioWait = row.ioWait === null ? undefined : !!row.ioWait;
    const title = translateState(row.state, ioWait);
    const color = colorForState(title);
    return {...baseSlice, title, colorScheme: color};
  }

  onUpdatedSlices(slices: Slice[]) {
    for (const slice of slices) {
      slice.isHighlighted = slice === this.hoveredSlice;
    }
  }

  onSliceClick(args: OnSliceClickArgs<Slice>) {
    this.trace.selection.selectTrackEvent(this.uri, args.slice.id);
  }

  // Add utid to selection details
  override async getSelectionDetails(
    id: number,
  ): Promise<TrackEventDetails | undefined> {
    const details = await super.getSelectionDetails(id);
    return details && {...details, utid: this.utid};
  }
}
