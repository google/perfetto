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
import {createTraceProcessorSliceTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_slice_track';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';

// This plugin renders visualizations of subsystems of the Linux kernel.
export default class implements PerfettoPlugin {
  static readonly id = 'org.kernel.LinuxKernelSubsystems';
  static readonly dependencies = [TraceProcessorTrackPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const kernel = new TrackNode({
      name: 'Linux Kernel',
      isSummary: true,
    });
    const rpm = await this.addRpmTracks(ctx);
    if (rpm.hasChildren) {
      ctx.defaultWorkspace.addChildInOrder(kernel);
      kernel.addChildInOrder(rpm);
    }
  }

  // Add tracks to visualize the runtime power state transitions for Linux
  // kernel devices (devices managed by Linux drivers).
  async addRpmTracks(ctx: Trace) {
    const result = await ctx.engine.query(`
      select
        t.id as trackId,
        extract_arg(t.dimension_arg_set_id, 'linux_device') as deviceName
      from track t
      join _slice_track_summary using (id)
      where type = 'linux_rpm'
      order by deviceName;
    `);

    const it = result.iter({
      deviceName: STR_NULL,
      trackId: NUM,
    });
    const rpm = new TrackNode({
      name: 'Runtime Power Management',
      isSummary: true,
    });
    for (; it.valid(); it.next()) {
      const trackId = it.trackId;
      const name = it.deviceName ?? `${trackId}`;
      const uri = `/linux/rpm/${name}`;
      ctx.tracks.registerTrack({
        uri,
        renderer: await createTraceProcessorSliceTrack({
          trace: ctx,
          uri,
          trackIds: [trackId],
        }),
        tags: {
          kinds: [SLICE_TRACK_KIND],
          trackIds: [trackId],
        },
      });
      const track = new TrackNode({uri, name: name});
      rpm.addChildInOrder(track);
    }
    return rpm;
  }
}
