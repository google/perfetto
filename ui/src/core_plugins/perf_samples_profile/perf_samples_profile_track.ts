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

import {Slice} from '../../public';
import {
  BaseSliceTrack,
  OnSliceClickArgs,
} from '../../frontend/base_slice_track';
import {NewTrackArgs} from '../../frontend/track';
import {NAMED_ROW, NamedRow} from '../../frontend/named_slice_track';
import {getColorForSlice} from '../../core/colorizer';
import {Time} from '../../base/time';
import {globals} from '../../frontend/globals';
import {Actions} from '../../common/actions';
import {LegacySelection, ProfileType} from '../../core/selection_manager';

abstract class BasePerfSamplesProfileTrack extends BaseSliceTrack<
  Slice,
  NamedRow
> {
  constructor(args: NewTrackArgs) {
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
    return selection.kind === 'PERF_SAMPLES';
  }

  onUpdatedSlices(slices: Slice[]) {
    for (const slice of slices) {
      slice.isHighlighted = slice === this.hoveredSlice;
    }
  }
}

export class ProcessPerfSamplesProfileTrack extends BasePerfSamplesProfileTrack {
  constructor(
    args: NewTrackArgs,
    private upid: number,
  ) {
    super(args);
  }

  getSqlSource(): string {
    return `
      select p.id, ts, 0 as dur, 0 as depth, 'Perf Sample' as name
      from perf_sample p
      join thread using (utid)
      where upid = ${this.upid}
        and callsite_id is not null
      order by ts
    `;
  }

  onSliceClick({slice}: OnSliceClickArgs<Slice>) {
    globals.makeSelection(
      Actions.selectPerfSamples({
        id: slice.id,
        upid: this.upid,
        leftTs: Time.fromRaw(slice.ts),
        rightTs: Time.fromRaw(slice.ts),
        type: ProfileType.PERF_SAMPLE,
      }),
    );
  }
}

export class ThreadPerfSamplesProfileTrack extends BasePerfSamplesProfileTrack {
  constructor(
    args: NewTrackArgs,
    private utid: number,
  ) {
    super(args);
  }

  getSqlSource(): string {
    return `
      select p.id, ts, 0 as dur, 0 as depth, 'Perf Sample' as name
      from perf_sample p
      where utid = ${this.utid}
        and callsite_id is not null
      order by ts
    `;
  }

  onSliceClick({slice}: OnSliceClickArgs<Slice>) {
    globals.makeSelection(
      Actions.selectPerfSamples({
        id: slice.id,
        utid: this.utid,
        leftTs: Time.fromRaw(slice.ts),
        rightTs: Time.fromRaw(slice.ts),
        type: ProfileType.PERF_SAMPLE,
      }),
    );
  }
}
