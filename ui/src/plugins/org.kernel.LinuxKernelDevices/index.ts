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
  NUM,
  Plugin,
  PluginContextTrace,
  PluginDescriptor,
  STR_NULL,
} from '../../public';
import {AsyncSliceTrack} from '../../core_plugins/async_slices/async_slice_track';
import {ASYNC_SLICE_TRACK_KIND} from '../../public';

// This plugin renders visualizations of runtime power state transitions for
// Linux kernel devices (devices managed by Linux drivers).
class LinuxKernelDevices implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
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
      const displayName = it.name ?? `${trackId}`;

      ctx.registerStaticTrack({
        uri: `org.kernel.LinuxKernelDevices#${displayName}`,
        displayName,
        trackIds: [trackId],
        kind: ASYNC_SLICE_TRACK_KIND,
        trackFactory: ({trackKey}) => {
          return new AsyncSliceTrack(
            {
              engine: ctx.engine,
              trackKey,
            },
            0,
            [trackId],
          );
        },
        groupName: `Linux Kernel Devices`,
      });
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'org.kernel.LinuxKernelDevices',
  plugin: LinuxKernelDevices,
};
