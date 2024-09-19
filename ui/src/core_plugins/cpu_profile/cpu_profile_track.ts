// Copyright (C) 2024 The Android Open Source Project
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

import {Time} from '../../base/time';
import {Actions} from '../../common/actions';
import {LegacySelection} from '../../common/state';
import {getColorForSlice} from '../../core/colorizer';
import {
  BaseSliceTrack,
  OnSliceClickArgs,
} from '../../frontend/base_slice_track';
import {globals} from '../../frontend/globals';
import {NAMED_ROW, NamedRow} from '../../frontend/named_slice_track';
import {NewTrackArgs} from '../../frontend/track';
import {Slice} from '../../public';

export class CpuProfileTrack extends BaseSliceTrack<Slice, NamedRow> {
  constructor(
    args: NewTrackArgs,
    private utid: number,
  ) {
    super(args);
  }

  protected getRowSpec(): NamedRow {
    return NAMED_ROW;
  }

  protected rowToSlice(row: NamedRow): Slice {
    const baseSlice = super.rowToSliceBase(row);
    const name = row.name ?? '';
    const colorScheme = getColorForSlice(name);
    return {...baseSlice, title: name, colorScheme};
  }

  isSelectionHandled(selection: LegacySelection): boolean {
    return selection.kind === 'CPU_PROFILE_SAMPLE';
  }

  onUpdatedSlices(slices: Slice[]) {
    for (const slice of slices) {
      slice.isHighlighted = slice === this.hoveredSlice;
    }
  }

  getSqlSource(): string {
    return `
      select p.id, ts, 0 as dur, 0 as depth, 'CPU Sample' as name
      from cpu_profile_stack_sample p
      where utid = ${this.utid}
      order by ts
    `;
  }

  onSliceClick({slice}: OnSliceClickArgs<Slice>) {
    globals.makeSelection(
      Actions.selectCpuProfileSample({
        id: slice.id,
        utid: this.utid,
        ts: Time.fromRaw(slice.ts),
      }),
    );
  }
}
