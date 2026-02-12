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

import {TrackNode} from '../../public/workspace';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {NUM, STR_NULL} from '../../trace_processor/query_result';
import {TraceProcessorCounterTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_counter_track';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack/index';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups/index';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.GpuFreq';
  static readonly dependencies = [
    TraceProcessorTrackPlugin,
    StandardGroupsPlugin,
  ];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const result = await ctx.engine.query(`
      select id, gpu_id as gpuId, unit
      from gpu_counter_track
      join _counter_track_summary using (id)
      where name = 'gpufreq'
    `);

    const tracks: Array<{id: number; gpuId: number; unit: string | null}> = [];
    const it = result.iter({id: NUM, gpuId: NUM, unit: STR_NULL});
    for (; it.valid(); it.next()) {
      tracks.push({id: it.id, gpuId: it.gpuId, unit: it.unit});
    }

    if (tracks.length === 0) return;

    const gpuGroup = ctx.plugins
      .getPlugin(StandardGroupsPlugin)
      .getOrCreateStandardGroup(ctx.defaultWorkspace, 'GPU');

    // Only create a sub-group if there's more than one track.
    let parent: TrackNode;
    if (tracks.length > 1) {
      parent = new TrackNode({
        name: 'GPU Frequency',
        isSummary: true,
      });
      gpuGroup.addChildInOrder(parent);
    } else {
      parent = gpuGroup;
    }

    for (const {id, gpuId, unit} of tracks) {
      const uri = `/gpu_frequency_${gpuId}`;
      const name = `Gpu ${gpuId} Frequency`;
      ctx.tracks.registerTrack({
        uri,
        tags: {
          kinds: [COUNTER_TRACK_KIND],
          trackIds: [id],
        },
        renderer: new TraceProcessorCounterTrack(
          ctx,
          uri,
          {unit: unit ?? undefined},
          id,
          name,
        ),
      });
      const track = new TrackNode({uri, name, sortOrder: -20});
      parent.addChildInOrder(track);
    }
  }
}
