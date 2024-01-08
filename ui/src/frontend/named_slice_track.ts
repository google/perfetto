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

import {Actions} from '../common/actions';
import {
  getColorForSlice,
} from '../common/colorizer';
import {STR_NULL} from '../trace_processor/query_result';

import {
  BASE_ROW,
  BaseSliceTrack,
  BaseSliceTrackTypes,
  OnSliceClickArgs,
  OnSliceOverArgs,
  SLICE_FLAGS_INCOMPLETE,
  SLICE_FLAGS_INSTANT,
} from './base_slice_track';
import {globals} from './globals';
import {NewTrackArgs} from './track';
import {renderDuration} from './widgets/duration';

export const NAMED_ROW = {
  // Base columns (tsq, ts, dur, id, depth).
  ...BASE_ROW,

  // Impl-specific columns.
  name: STR_NULL,
};
export type NamedRow = typeof NAMED_ROW;

export interface NamedSliceTrackTypes extends BaseSliceTrackTypes {
  row: NamedRow;
}

export abstract class NamedSliceTrack<
    T extends NamedSliceTrackTypes = NamedSliceTrackTypes> extends
    BaseSliceTrack<T> {
  constructor(args: NewTrackArgs) {
    super(args);
  }

  // This is used by the base class to call iter().
  getRowSpec(): T['row'] {
    return NAMED_ROW;
  }

  // Converts a SQL result row to an "Impl" Slice.
  rowToSlice(row: T['row']): T['slice'] {
    const baseSlice = super.rowToSlice(row);
    // Ignore PIDs or numeric arguments when hashing.
    const name = row.name || '';
    const colorScheme = getColorForSlice(name);
    return {...baseSlice, title: name, colorScheme};
  }

  onSliceOver(args: OnSliceOverArgs<T['slice']>) {
    const {title, dur, flags} = args.slice;
    let duration;
    if (flags & SLICE_FLAGS_INCOMPLETE) {
      duration = 'Incomplete';
    } else if (flags & SLICE_FLAGS_INSTANT) {
      duration = 'Instant';
    } else {
      duration = renderDuration(dur);
    }
    args.tooltip = [`${title} - [${duration}]`];
  }

  onSliceClick(args: OnSliceClickArgs<T['slice']>) {
    globals.makeSelection(Actions.selectChromeSlice({
      id: args.slice.id,
      trackKey: this.trackKey,

      // |table| here can be either 'slice' or 'annotation'. The
      // AnnotationSliceTrack overrides the onSliceClick and sets this to
      // 'annotation'
      table: 'slice',
    }));
  }
}
