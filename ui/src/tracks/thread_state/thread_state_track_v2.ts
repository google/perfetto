// Copyright (C) 2021 The Android Open Source Project
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

import {Actions} from '../../common/actions';
import {Color, colorForState} from '../../common/colorizer';
import {NUM_NULL, STR} from '../../common/query_result';
import {Selection} from '../../common/state';
import {translateState} from '../../common/thread_state';
import {
  BASE_SLICE_ROW,
  BaseSliceTrack,
  BaseSliceTrackTypes,
  OnSliceClickArgs,
} from '../../frontend/base_slice_track';
import {globals} from '../../frontend/globals';
import {
  SLICE_LAYOUT_FLAT_DEFAULTS,
  SliceLayout,
} from '../../frontend/slice_layout';
import {NewTrackArgs} from '../../frontend/track';

export const THREAD_STATE_ROW = {
  ...BASE_SLICE_ROW,
  state: STR,
  ioWait: NUM_NULL,
};

export type ThreadStateRow = typeof THREAD_STATE_ROW;

export interface ThreadStateTrackConfig {
  utid: number;
}

export interface ThreadStateTrackTypes extends BaseSliceTrackTypes {
  row: ThreadStateRow;
  config: ThreadStateTrackConfig;
}

export class ThreadStateTrack extends BaseSliceTrack<ThreadStateTrackTypes> {
  static create(args: NewTrackArgs) {
    return new ThreadStateTrack(args);
  }

  protected sliceLayout: SliceLayout = {...SLICE_LAYOUT_FLAT_DEFAULTS};

  constructor(args: NewTrackArgs) {
    super(args);
  }

  // This is used by the base class to call iter().
  getRowSpec(): ThreadStateTrackTypes['row'] {
    return THREAD_STATE_ROW;
  }

  getSqlSource(): string {
    // Do not display states 'x' and 'S' (dead & sleeping).
    const sql = `
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
        utid = ${this.config.utid} and
        state != 'x' and
        state != 'S'
    `;
    return sql;
  }

  rowToSlice(row: ThreadStateTrackTypes['row']):
      ThreadStateTrackTypes['slice'] {
    const baseSlice = super.rowToSlice(row);
    const ioWait = row.ioWait === null ? undefined : !!row.ioWait;
    const title = translateState(row.state, ioWait);
    const baseColor: Color = colorForState(title);
    return {...baseSlice, title, baseColor};
  }

  onUpdatedSlices(slices: ThreadStateTrackTypes['slice'][]) {
    for (const slice of slices) {
      if (slice === this.hoveredSlice) {
        slice.color = {
          c: slice.baseColor.c,
          h: slice.baseColor.h,
          s: slice.baseColor.s,
          l: 30,
        };
      } else {
        slice.color = slice.baseColor;
      }
    }
  }

  onSliceClick(args: OnSliceClickArgs<ThreadStateTrackTypes['slice']>) {
    globals.makeSelection(Actions.selectThreadState({
      id: args.slice.id,
      trackKey: this.trackKey,
    }));
  }

  protected isSelectionHandled(selection: Selection): boolean {
    return selection.kind === 'THREAD_STATE';
  }
}
