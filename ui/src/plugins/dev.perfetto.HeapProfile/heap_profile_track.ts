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

import {Duration, Time} from '../../base/time';
import {
  BASE_ROW,
  BaseSliceTrack,
  OnSliceOverArgs,
} from '../../components/tracks/base_slice_track';
import {
  ProfileType,
  profileType,
  TrackEventDetails,
  TrackEventSelection,
} from '../../public/selection';
import {Trace} from '../../public/trace';
import {Slice} from '../../public/track';
import {LONG, STR} from '../../trace_processor/query_result';
import {HeapProfileFlamegraphDetailsPanel} from './heap_profile_details_panel';

const HEAP_PROFILE_ROW = {
  ...BASE_ROW,
  type: STR,
};
type HeapProfileRow = typeof HEAP_PROFILE_ROW;
interface HeapProfileSlice extends Slice {
  type: ProfileType;
}

export class HeapProfileTrack extends BaseSliceTrack<
  HeapProfileSlice,
  HeapProfileRow
> {
  constructor(
    trace: Trace,
    uri: string,
    private readonly tableName: string,
    private readonly upid: number,
    private readonly heapProfileIsIncomplete: boolean,
  ) {
    super(trace, uri, HEAP_PROFILE_ROW);
  }

  getSqlSource(): string {
    return this.tableName;
  }

  rowToSlice(row: HeapProfileRow): HeapProfileSlice {
    const slice = this.rowToSliceBase(row);
    return {
      ...slice,
      type: profileType(row.type),
    };
  }

  onSliceOver(args: OnSliceOverArgs<HeapProfileSlice>) {
    args.tooltip = [args.slice.type];
  }

  async getSelectionDetails(
    id: number,
  ): Promise<TrackEventDetails | undefined> {
    const query = `
      SELECT
        ts,
        dur,
        type
      FROM (${this.getSqlSource()})
      WHERE id = ${id}
    `;

    const result = await this.engine.query(query);
    if (result.numRows() === 0) {
      return undefined;
    }

    const row = result.iter({
      ts: LONG,
      dur: LONG,
      type: STR,
    });

    return {
      ts: Time.fromRaw(row.ts),
      dur: Duration.fromRaw(row.dur),
      profileType: profileType(row.type),
    };
  }

  detailsPanel(sel: TrackEventSelection) {
    return new HeapProfileFlamegraphDetailsPanel(
      this.trace,
      this.heapProfileIsIncomplete,
      this.upid,
      sel,
    );
  }
}
