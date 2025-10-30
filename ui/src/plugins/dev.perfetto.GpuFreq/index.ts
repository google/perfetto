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
import {NUM} from '../../trace_processor/query_result';
import {TraceProcessorCounterTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_counter_track';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack/index';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.GpuFreq';
  static readonly dependencies = [TraceProcessorTrackPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const result = await ctx.engine.query(`
      select id, gpu_id as gpuId
      from gpu_counter_track
      join _counter_track_summary using (id)
      where name = 'gpufreq'
    `);
    const it = result.iter({id: NUM, gpuId: NUM});
    for (; it.valid(); it.next()) {
      const uri = `/gpu_frequency_${it.gpuId}`;
      const name = `Gpu ${it.gpuId} Frequency`;
      ctx.tracks.registerTrack({
        uri,
        tags: {
          kinds: [COUNTER_TRACK_KIND],
          trackIds: [it.id],
        },
        renderer: new TraceProcessorCounterTrack(ctx, uri, {}, it.id, name),
      });
      const track = new TrackNode({uri, name, sortOrder: -20});
      ctx.defaultWorkspace.addChildInOrder(track);
    }
  }
}
