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

import {NUM, STR_NULL} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {AsyncSliceTrack} from '../dev.perfetto.AsyncSlices/async_slice_track';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';
import AsyncSlicesPlugin from '../dev.perfetto.AsyncSlices';

// This plugin renders visualizations of subsystems of the Linux kernel.
export default class implements PerfettoPlugin {
  static readonly id = 'org.kernel.LinuxKernelSubsystems';
  static readonly dependencies = [AsyncSlicesPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const kernel = new TrackNode({
      title: 'Linux Kernel',
      isSummary: true,
    });
    const rpm = await this.addRpmTracks(ctx);
    if (rpm.hasChildren) {
      ctx.workspace.addChildInOrder(kernel);
      kernel.addChildInOrder(rpm);
    }
  }

  // Add tracks to visualize the runtime power state transitions for Linux
  // kernel devices (devices managed by Linux drivers).
  async addRpmTracks(ctx: Trace) {
    const result = await ctx.engine.query(`
      select
        t.id as trackId,
        extract_arg(t.dimension_arg_set_id, 'linux_device_name') as deviceName
      from track t
      join _slice_track_summary using (id)
      where classification = 'linux_rpm'
      order by deviceName;
    `);

    const it = result.iter({
      deviceName: STR_NULL,
      trackId: NUM,
    });
    const rpm = new TrackNode({
      title: 'Runtime Power Management',
      isSummary: true,
    });
    for (; it.valid(); it.next()) {
      const trackId = it.trackId;
      const title = it.deviceName ?? `${trackId}`;

      const uri = `/linux/rpm/${title}`;
      ctx.tracks.registerTrack({
        uri,
        title,
        track: new AsyncSliceTrack(
          {
            trace: ctx,
            uri,
          },
          0,
          [trackId],
        ),
        tags: {
          kind: SLICE_TRACK_KIND,
          trackIds: [trackId],
          groupName: `Linux Kernel Devices`,
        },
      });
      const track = new TrackNode({uri, title});
      rpm.addChildInOrder(track);
    }
    return rpm;
  }
}
