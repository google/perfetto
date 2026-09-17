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

import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';
import type {Trace} from '../../public/trace';
import type {PerfettoPlugin} from '../../public/plugin';
import {createTraceProcessorSliceTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_slice_track';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';
import {getMachineCount, getTrackName} from '../../public/utils';

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
    const numMachines = await getMachineCount(ctx.engine);
    const result = await ctx.engine.query(`
      with rpm_tracks as (
        select
          t.id,
          t.machine_id,
          extract_arg(t.dimension_arg_set_id, 'linux_device') as deviceName,
          coalesce(
            extract_arg(t.dimension_arg_set_id, 'linux_device'),
            'track_' || t.id
          ) as deviceKey
        from track t
        join _slice_track_summary using (id)
        where type = 'linux_rpm'
      )
      select
        t.machine_id as machineId,
        machine.name as machineName,
        machine.label_index as machineLabelIndex,
        min(t.deviceName) as deviceName,
        group_concat(t.id) as trackIds,
        __max_layout_depth(count(), group_concat(t.id)) as maxDepth
      from rpm_tracks t
      left join machine on machine.id = t.machine_id
      group by t.machine_id, t.deviceKey
      order by t.machine_id, lower(t.deviceName);
    `);

    const it = result.iter({
      machineId: NUM,
      machineName: STR_NULL,
      machineLabelIndex: NUM_NULL,
      deviceName: STR_NULL,
      trackIds: STR,
      maxDepth: NUM,
    });
    const rpm = new TrackNode({
      name: 'Runtime Power Management',
      isSummary: true,
    });
    for (; it.valid(); it.next()) {
      const trackIds = it.trackIds.split(',').map(Number);
      const rawName = it.deviceName ?? `Track ${trackIds[0]}`;
      const name = getTrackName({
        name: rawName,
        machineName: it.machineName,
        machineLabelIndex: it.machineLabelIndex,
        numMachines,
      });
      const uri = `/linux/rpm/${it.machineId}/${encodeURIComponent(rawName)}`;
      ctx.tracks.registerTrack({
        uri,
        renderer: await createTraceProcessorSliceTrack({
          trace: ctx,
          uri,
          trackIds,
          maxDepth: it.maxDepth,
        }),
        tags: {
          kinds: [SLICE_TRACK_KIND],
          trackIds,
        },
      });
      const track = new TrackNode({uri, name: name});
      rpm.addChildInOrder(track);
    }
    return rpm;
  }
}
