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
  Color,
  hslForSlice,
} from '../common/colorizer';
import {STR_NULL} from '../common/query_result';

import {
  BASE_SLICE_ROW,
  BaseSliceTrack,
  BaseSliceTrackTypes,
  OnSliceClickArgs,
  OnSliceOverArgs,
} from './base_slice_track';
import {globals} from './globals';
import {NewTrackArgs} from './track';

export const NAMED_SLICE_ROW = {
  // Base columns (tsq, ts, dur, id, depth).
  ...BASE_SLICE_ROW,

  // Impl-specific columns.
  name: STR_NULL,
};
export type NamedSliceRow = typeof NAMED_SLICE_ROW;

export interface NamedSliceTrackTypes extends BaseSliceTrackTypes {
  row: NamedSliceRow;
}

export abstract class NamedSliceTrack<
    T extends NamedSliceTrackTypes = NamedSliceTrackTypes> extends
    BaseSliceTrack<T> {
  constructor(args: NewTrackArgs) {
    super(args);
  }

  // This is used by the base class to call iter().
  getRowSpec(): T['row'] {
    return NAMED_SLICE_ROW;
  }

  // Converts a SQL result row to an "Impl" Slice.
  rowToSlice(row: T['row']): T['slice'] {
    const baseSlice = super.rowToSlice(row);
    // Ignore PIDs or numeric arguments when hashing.
    const name = row.name || '';
    const nameForHashing = name.replace(/\s?\d+/g, '');
    const hsl = hslForSlice(nameForHashing, /* isSelected=*/ false);
    // We cache the color so we hash only once per query.
    const baseColor: Color = {c: '', h: hsl[0], s: hsl[1], l: hsl[2]};
    return {...baseSlice, title: name, baseColor};
  }

  onSliceOver(args: OnSliceOverArgs<T['slice']>) {
    const name = args.slice.title;
    args.tooltip = [name];
  }

  onSliceClick(args: OnSliceClickArgs<T['slice']>) {
    globals.makeSelection(Actions.selectChromeSlice({
      id: args.slice.id,
      trackId: this.trackId,

      // |table| here can be either 'slice' or 'annotation'. The
      // AnnotationSliceTrack overrides the onSliceClick and sets this to
      // 'annotation'
      table: 'slice',
    }));
  }
}
