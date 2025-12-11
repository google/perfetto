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
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {TrackNode} from '../../public/workspace';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {ThreadSliceDetailsPanel} from '../../components/details/thread_slice_details_tab';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.GpuByProcess';
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
      pid: LONG_NULL,
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
      ctx.tracks.registerTrack({
        uri,
        renderer: SliceTrack.create({
          trace: ctx,
          uri,
          dataset: new SourceDataset({
            src: 'gpu_slice',
            schema: {
              id: NUM,
              name: STR,
              ts: LONG,
              dur: LONG,
              depth: NUM,
              upid: NUM,
            },
            filter: {
              col: 'upid',
              eq: upid,
            },
          }),
          detailsPanel: () => new ThreadSliceDetailsPanel(ctx),
        }),
      });
      const track = new TrackNode({
        uri,
        name: `GPU ${processName}`,
      });
      ctx.defaultWorkspace.addChildInOrder(track);
    }
  }
}
