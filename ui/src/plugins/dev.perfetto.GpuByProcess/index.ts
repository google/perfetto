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

import {NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {Slice} from '../../public/track';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {
  NAMED_ROW,
  NamedRow,
  NamedSliceTrack,
} from '../../frontend/named_slice_track';
import {NewTrackArgs} from '../../frontend/track';
import {TrackNode} from '../../public/workspace';
class GpuPidTrack extends NamedSliceTrack {
  upid: number;

  constructor(args: NewTrackArgs, upid: number) {
    super(args);
    this.upid = upid;
  }

  protected getRowSpec(): NamedRow {
    return NAMED_ROW;
  }

  protected rowToSlice(row: NamedRow): Slice {
    return this.rowToSliceBase(row);
  }

  getSqlSource(): string {
    return `
      SELECT *
      FROM gpu_slice
      WHERE upid = ${this.upid}
    `;
  }
}

class GpuByProcess implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    // Find all unique upid values in gpu_slices and join with process table.
    const results = await ctx.engine.query(`
      WITH slice_upids AS (
        SELECT DISTINCT upid FROM gpu_slice
      )
      SELECT upid, pid, name FROM slice_upids JOIN process USING (upid)
    `);

    const it = results.iter({
      upid: NUM_NULL,
      pid: NUM_NULL,
      name: STR_NULL,
    });

    // For each upid, create a GpuPidTrack.
    for (; it.valid(); it.next()) {
      if (it.upid == null) {
        continue;
      }

      const upid = it.upid;
      let processName = 'Unknown';
      if (it.name != null) {
        processName = it.name;
      } else if (it.pid != null) {
        processName = `${it.pid}`;
      }

      const uri = `dev.perfetto.GpuByProcess#${upid}`;
      const title = `GPU ${processName}`;
      ctx.tracks.registerTrack({
        uri,
        title,
        track: new GpuPidTrack({trace: ctx, uri}, upid),
      });
      const track = new TrackNode({uri, title});
      track.uri = uri;
      track.title = title;
      ctx.workspace.addChildInOrder(track);
    }
  }

  async onTraceUnload(_: Trace): Promise<void> {}
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.GpuByProcess',
  plugin: GpuByProcess,
};
