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

import {assertExists} from '../../base/logging';
import {TrackEventDetails} from '../../public/selection';
import {getColorForSample} from '../../core/colorizer';
import {
  BaseSliceTrack,
  OnSliceClickArgs,
} from '../../frontend/base_slice_track';
import {NAMED_ROW, NamedRow} from '../../frontend/named_slice_track';
import {NewTrackArgs} from '../../frontend/track';
import {NUM} from '../../trace_processor/query_result';
import {Slice} from '../../public/track';

interface CpuProfileRow extends NamedRow {
  callsiteId: number;
}

export class CpuProfileTrack extends BaseSliceTrack<Slice, CpuProfileRow> {
  constructor(
    args: NewTrackArgs,
    private utid: number,
  ) {
    super(args);
  }

  protected getRowSpec(): CpuProfileRow {
    return {...NAMED_ROW, callsiteId: NUM};
  }

  protected rowToSlice(row: CpuProfileRow): Slice {
    const baseSlice = super.rowToSliceBase(row);
    const name = assertExists(row.name);
    const colorScheme = getColorForSample(row.callsiteId);
    return {...baseSlice, title: name, colorScheme};
  }

  onUpdatedSlices(slices: Slice[]) {
    for (const slice of slices) {
      slice.isHighlighted = slice === this.hoveredSlice;
    }
  }

  getSqlSource(): string {
    return `
      select
        p.id,
        ts,
        0 as dur,
        0 as depth,
        'CPU Sample' as name,
        callsite_id as callsiteId
      from cpu_profile_stack_sample p
      where utid = ${this.utid}
      order by ts
    `;
  }

  onSliceClick({slice}: OnSliceClickArgs<Slice>) {
    this.trace.selection.selectTrackEvent(this.uri, slice.id);
  }

  async getSelectionDetails(
    id: number,
  ): Promise<TrackEventDetails | undefined> {
    const baseDetails = await super.getSelectionDetails(id);
    if (baseDetails === undefined) return undefined;
    return {...baseDetails, utid: this.utid};
  }
}
