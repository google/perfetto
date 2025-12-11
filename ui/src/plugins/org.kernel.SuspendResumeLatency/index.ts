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
import {createTraceProcessorSliceTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_slice_track';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {SuspendResumeDetailsPanel} from './suspend_resume_details';
import ThreadPlugin from '../dev.perfetto.Thread';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';

export default class implements PerfettoPlugin {
  static readonly id = 'org.kernel.SuspendResumeLatency';
  static readonly dependencies = [ThreadPlugin, TraceProcessorTrackPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const threads = ctx.plugins.getPlugin(ThreadPlugin).getThreadMap();
    const {engine} = ctx;
    const rawGlobalAsyncTracks = await engine.query(`
      with global_tracks_grouped as (
        select
          name,
          group_concat(distinct t.id) as trackIds,
          count() as trackCount
        from track t
        where t.type = 'suspend_resume'
      )
      select
        t.trackIds as trackIds,
        case
          when
            t.trackCount > 0
          then
            __max_layout_depth(t.trackCount, t.trackIds)
          else 0
        end as maxDepth
      from global_tracks_grouped t
    `);
    const it = rawGlobalAsyncTracks.iter({
      trackIds: STR_NULL,
      maxDepth: NUM,
    });
    // If no Suspend/Resume tracks exist, then nothing to do.
    if (it.trackIds == null) {
      return;
    }
    const rawTrackIds = it.trackIds;
    const trackIds = rawTrackIds.split(',').map((v) => Number(v));
    const maxDepth = it.maxDepth;

    const uri = `/suspend_resume_latency`;
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
    const track = new TrackNode({uri, name: 'Suspend/Resume Latency'});
    ctx.defaultWorkspace.addChildInOrder(track);
  }
}
