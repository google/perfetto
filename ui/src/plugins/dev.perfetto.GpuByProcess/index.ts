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

import {
  NUM_NULL,
  Plugin,
  PluginContextTrace,
  PluginDescriptor,
  STR_NULL,
} from '../../public';
import {NamedSliceTrack} from '../../frontend/named_slice_track';
import {NewTrackArgs} from '../../frontend/track';

class GpuPidTrack extends NamedSliceTrack {
  upid: number;

  constructor(args: NewTrackArgs, upid: number) {
    super(args);
    this.upid = upid;
  }

  getSqlSource(): string {
    return `
      SELECT *
      FROM gpu_slice
      WHERE upid = ${this.upid}
    `;
  }
}

class GpuByProcess implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
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

      ctx.registerStaticTrack({
        uri: `dev.perfetto.GpuByProcess#${upid}`,
        displayName: `GPU ${processName}`,
        trackFactory: ({trackKey}) => {
          return new GpuPidTrack({engine: ctx.engine, trackKey}, upid);
        },
      });
    }
  }

  async onTraceUnload(_: PluginContextTrace): Promise<void> {}
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.GpuByProcess',
  plugin: GpuByProcess,
};
