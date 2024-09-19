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

import {
  BASE_ROW,
  BaseSliceTrack,
  OnSliceClickArgs,
  OnSliceOverArgs,
} from '../../frontend/base_slice_track';
import {globals} from '../../frontend/globals';
import {NewTrackArgs} from '../../frontend/track';
import {
  LegacySelection,
  ProfileType,
  profileType,
} from '../../public/selection';
import {Slice} from '../../public/track';
import {STR} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';

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
  private upid: number;

  constructor(args: NewTrackArgs, upid: number) {
    super(args);
    this.upid = upid;
  }

  async onInit() {
    return createPerfettoTable(
      this.engine,
      `_heap_profile_track_${this.trackUuid}`,
      `
      with
        heaps as (select group_concat(distinct heap_name) h from heap_profile_allocation where upid = ${this.upid}),
        allocation_tses as (select distinct ts from heap_profile_allocation where upid = ${this.upid}),
        graph_tses as (select distinct graph_sample_ts from heap_graph_object where upid = ${this.upid})
      select
        *,
        0 AS dur,
        0 AS depth
      from (
        select
          (
            select a.id
            from heap_profile_allocation a
            where a.ts = t.ts
            order by a.id
            limit 1
          ) as id,
          ts,
          'heap_profile:' || (select h from heaps) AS type
        from allocation_tses t
        union all
        select
          (
            select o.id
            from heap_graph_object o
            where o.graph_sample_ts = g.graph_sample_ts
            order by o.id
            limit 1
          ) as id,
          graph_sample_ts AS ts,
          'graph' AS type
        from graph_tses g
      )
    `,
    );
  }

  getSqlSource(): string {
    return `_heap_profile_track_${this.trackUuid}`;
  }

  getRowSpec(): HeapProfileRow {
    return HEAP_PROFILE_ROW;
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

  onSliceClick(args: OnSliceClickArgs<HeapProfileSlice>) {
    globals.selectionManager.setHeapProfile({
      id: args.slice.id,
      upid: this.upid,
      ts: args.slice.ts,
      type: args.slice.type,
    });
  }

  protected isSelectionHandled(selection: LegacySelection): boolean {
    return selection.kind === 'HEAP_PROFILE';
  }
}
