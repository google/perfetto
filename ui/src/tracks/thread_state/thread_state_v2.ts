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

import {Actions} from '../../common/actions';
import {colorForState} from '../../common/colorizer';
import {Selection} from '../../common/state';
import {translateState} from '../../common/thread_state';
import {
  BASE_ROW,
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
import {NUM_NULL, STR} from '../../trace_processor/query_result';

export const THREAD_STATE_ROW = {
  ...BASE_ROW,
  state: STR,
  ioWait: NUM_NULL,
};

export type ThreadStateRow = typeof THREAD_STATE_ROW;

export interface ThreadStateTrackTypes extends BaseSliceTrackTypes {
  row: ThreadStateRow;
}

export class ThreadStateTrack extends BaseSliceTrack<ThreadStateTrackTypes> {
  protected sliceLayout: SliceLayout = {...SLICE_LAYOUT_FLAT_DEFAULTS};

  constructor(args: NewTrackArgs, private utid: number) {
    super(args);
  }

  // This is used by the base class to call iter().
  getRowSpec(): ThreadStateTrackTypes['row'] {
    return THREAD_STATE_ROW;
  }

  getSqlSource(): string {
    // Do not display states 'x' and 'S' (dead & sleeping).
    // Note: Thread state tracks V1 basically ignores incomplete slices, faking
    // their duration as 1 instead. Let's just do this here as well for now to
    // achieve feature parity with tracks V1 and tackle the issue of overlapping
    // incomplete slices later.
    return `
      select
        id,
        ts,
        max(dur, 1) as dur,
        cpu,
        state,
        io_wait as ioWait,
        0 as depth
      from thread_state
      where
        utid = ${this.utid} and
        state != 'x' and
        state != 'S'
    `;
  }

  rowToSlice(row: ThreadStateTrackTypes['row']):
      ThreadStateTrackTypes['slice'] {
    const baseSlice = super.rowToSlice(row);
    const ioWait = row.ioWait === null ? undefined : !!row.ioWait;
    const title = translateState(row.state, ioWait);
    const color = colorForState(title);
    return {...baseSlice, title, colorScheme: color};
  }

  onUpdatedSlices(slices: ThreadStateTrackTypes['slice'][]) {
    for (const slice of slices) {
      slice.isHighlighted = (slice === this.hoveredSlice);
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
