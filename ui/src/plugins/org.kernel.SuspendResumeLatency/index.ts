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
import {createTraceProcessorSliceTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_slice_track';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {SuspendResumeDetailsPanel} from './suspend_resume_details';
import ThreadPlugin from '../dev.perfetto.Thread';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';
import {getMachineCount, getTrackName} from '../../public/utils';

export default class implements PerfettoPlugin {
  static readonly id = 'org.kernel.SuspendResumeLatency';
  static readonly dependencies = [ThreadPlugin, TraceProcessorTrackPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const threads = ctx.plugins.getPlugin(ThreadPlugin).getThreadMap();
    const {engine} = ctx;
    const numMachines = await getMachineCount(engine);
    const rawGlobalAsyncTracks = await engine.query(`
      with global_tracks_grouped as (
        select
          t.machine_id as machineId,
          group_concat(distinct t.id) as trackIds,
          count() as trackCount
        from track t
        where t.type = 'suspend_resume'
        group by t.machine_id
      )
      select
        t.trackIds as trackIds,
        t.machineId as machineId,
        machine.name as machineName,
        machine.label_index as machineLabelIndex,
        case
          when
            t.trackCount > 0
          then
            __max_layout_depth(t.trackCount, t.trackIds)
          else 0
        end as maxDepth
      from global_tracks_grouped t
      left join machine on machine.id = t.machineId
      order by t.machineId
    `);
    const it = rawGlobalAsyncTracks.iter({
      trackIds: STR,
      machineId: NUM,
      machineName: STR_NULL,
      machineLabelIndex: NUM_NULL,
      maxDepth: NUM,
    });
    for (; it.valid(); it.next()) {
      const trackIds = it.trackIds.split(',').map((v) => Number(v));
      const maxDepth = it.maxDepth;

      const uri = `/suspend_resume_latency_${it.machineId}`;
      ctx.tracks.registerTrack({
        uri,
        tags: {
          trackIds,
          kinds: [SLICE_TRACK_KIND],
        },
        renderer: await createTraceProcessorSliceTrack({
          trace: ctx,
          uri,
          maxDepth,
          trackIds,
          detailsPanel: () => new SuspendResumeDetailsPanel(ctx, threads),
        }),
      });

      // Display the track in the UI.
      const name = getTrackName({
        name: 'Suspend/Resume Latency',
        machineName: it.machineName,
        machineLabelIndex: it.machineLabelIndex,
        numMachines,
      });
      const track = new TrackNode({uri, name});
      ctx.defaultWorkspace.addChildInOrder(track);
    }
  }
}
