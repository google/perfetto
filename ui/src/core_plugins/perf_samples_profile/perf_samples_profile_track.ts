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

import {NUM} from '../../trace_processor/query_result';
import {Slice} from '../../public/track';
import {
  BaseSliceTrack,
  OnSliceClickArgs,
} from '../../frontend/base_slice_track';
import {NewTrackArgs} from '../../frontend/track';
import {NAMED_ROW, NamedRow} from '../../frontend/named_slice_track';
import {getColorForSample} from '../../core/colorizer';
import {ProfileType, TrackEventDetails} from '../../public/selection';
import {assertExists} from '../../base/logging';

interface PerfSampleRow extends NamedRow {
  callsiteId: number;
}

abstract class BasePerfSamplesProfileTrack extends BaseSliceTrack<
  Slice,
  PerfSampleRow
> {
  constructor(args: NewTrackArgs) {
    super(args);
  }

  protected getRowSpec(): PerfSampleRow {
    return {...NAMED_ROW, callsiteId: NUM};
  }

  protected rowToSlice(row: PerfSampleRow): Slice {
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

  onSliceClick(args: OnSliceClickArgs<Slice>): void {
    // TODO(stevegolton): Perhaps we could just move this to BaseSliceTrack?
    this.trace.selection.selectTrackEvent(this.uri, args.slice.id);
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
      select
        p.id,
        ts,
        0 as dur,
        0 as depth,
        'Perf Sample' as name,
        callsite_id as callsiteId
      from perf_sample p
      join thread using (utid)
      where upid = ${this.upid} and callsite_id is not null
      order by ts
    `;
  }

  async getSelectionDetails(
    id: number,
  ): Promise<TrackEventDetails | undefined> {
    const details = await super.getSelectionDetails(id);
    if (details === undefined) return undefined;
    return {
      ...details,
      upid: this.upid,
      profileType: ProfileType.PERF_SAMPLE,
    };
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
      select
        p.id,
        ts,
        0 as dur,
        0 as depth,
        'Perf Sample' as name,
        callsite_id as callsiteId
      from perf_sample p
      where utid = ${this.utid} and callsite_id is not null
      order by ts
    `;
  }

  async getSelectionDetails(
    id: number,
  ): Promise<TrackEventDetails | undefined> {
    const details = await super.getSelectionDetails(id);
    if (details === undefined) return undefined;
    return {
      ...details,
      utid: this.utid,
      profileType: ProfileType.PERF_SAMPLE,
    };
  }
}
