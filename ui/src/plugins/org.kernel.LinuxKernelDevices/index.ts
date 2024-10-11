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
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {AsyncSliceTrack} from '../../core_plugins/async_slices/async_slice_track';
import {ASYNC_SLICE_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';

// This plugin renders visualizations of runtime power state transitions for
// Linux kernel devices (devices managed by Linux drivers).
class LinuxKernelDevices implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    const result = await ctx.engine.query(`
      select
        t.id as trackId,
        t.name
      from linux_device_track t
      join _slice_track_summary using (id)
      order by t.name;
    `);

    const it = result.iter({
      name: STR_NULL,
      trackId: NUM,
    });

    for (; it.valid(); it.next()) {
      const trackId = it.trackId;
      const title = it.name ?? `${trackId}`;

      const uri = `/kernel_devices/${title}`;
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
          kind: ASYNC_SLICE_TRACK_KIND,
          trackIds: [trackId],
          groupName: `Linux Kernel Devices`,
        },
      });
      const group = new TrackNode({
        title: 'Linux Kernel Devices',
        isSummary: true,
      });
      const track = new TrackNode({uri, title});
      group.addChildInOrder(track);
      ctx.workspace.addChildInOrder(group);
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'org.kernel.LinuxKernelDevices',
  plugin: LinuxKernelDevices,
};
