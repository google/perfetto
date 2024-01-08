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
import {ProfileType, Selection} from '../../common/state';
import {profileType} from '../../controller/flamegraph_controller';
import {
  BASE_ROW,
  BaseSliceTrack,
  BaseSliceTrackTypes,
  OnSliceClickArgs,
  OnSliceOverArgs,
} from '../../frontend/base_slice_track';
import {globals} from '../../frontend/globals';
import {NewTrackArgs} from '../../frontend/track';
import {
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
  Slice,
} from '../../public';
import {NUM, STR} from '../../trace_processor/query_result';

export const HEAP_PROFILE_TRACK_KIND = 'HeapProfileTrack';

const HEAP_PROFILE_ROW = {
  ...BASE_ROW,
  type: STR,
};
type HeapProfileRow = typeof HEAP_PROFILE_ROW;
interface HeapProfileSlice extends Slice {
  type: ProfileType;
}

interface HeapProfileTrackTypes extends BaseSliceTrackTypes {
  row: HeapProfileRow;
  slice: HeapProfileSlice;
}

class HeapProfileTrack extends BaseSliceTrack<HeapProfileTrackTypes> {
  private upid: number;

  constructor(args: NewTrackArgs, upid: number) {
    super(args);
    this.upid = upid;
  }

  getSqlSource(): string {
    return `select
      *,
      0 AS dur,
      0 AS depth
      from (
        select distinct
          id,
          ts,
          'heap_profile:' || (select group_concat(distinct heap_name) from heap_profile_allocation where upid = ${
        this.upid}) AS type
        from heap_profile_allocation
        where upid = ${this.upid}
        union
        select distinct
          id,
          graph_sample_ts AS ts,
          'graph' AS type
        from heap_graph_object
        where upid = ${this.upid}
      )`;
  }

  getRowSpec(): HeapProfileRow {
    return HEAP_PROFILE_ROW;
  }

  rowToSlice(row: HeapProfileRow): HeapProfileSlice {
    const slice = super.rowToSlice(row);
    let type = row.type;
    if (type === 'heap_profile:libc.malloc,com.android.art') {
      type = 'heap_profile:com.android.art,libc.malloc';
    }
    slice.type = profileType(type);
    return slice;
  }

  onSliceOver(args: OnSliceOverArgs<HeapProfileSlice>) {
    args.tooltip = [args.slice.type];
  }

  onSliceClick(args: OnSliceClickArgs<HeapProfileSlice>) {
    globals.makeSelection(Actions.selectHeapProfile({
      id: args.slice.id,
      upid: this.upid,
      ts: args.slice.ts,
      type: args.slice.type,
    }));
  }

  protected isSelectionHandled(selection: Selection): boolean {
    return selection.kind === 'HEAP_PROFILE';
  }
}

class HeapProfilePlugin implements Plugin {
  onActivate(_ctx: PluginContext): void {}
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const result = await ctx.engine.query(`
    select distinct(upid) from heap_profile_allocation
    union
    select distinct(upid) from heap_graph_object
  `);
    for (const it = result.iter({upid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      ctx.registerTrack({
        uri: `perfetto.HeapProfile#${upid}`,
        displayName: 'Heap Profile',
        kind: HEAP_PROFILE_TRACK_KIND,
        upid,
        track: ({trackKey}) => {
          return new HeapProfileTrack(
              {
                engine: ctx.engine,
                trackKey,
              },
              upid);
        },
      });
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.HeapProfile',
  plugin: HeapProfilePlugin,
};
