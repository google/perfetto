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
import {AsyncSliceTrack} from '../dev.perfetto.AsyncSlices/async_slice_track';
import {NewTrackArgs} from '../../frontend/track';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {SuspendResumeDetailsPanel} from './suspend_resume_details';
import {Slice} from '../../public/track';
import {OnSliceClickArgs} from '../../frontend/base_slice_track';
import {ThreadMap} from '../dev.perfetto.Thread/threads';
import ThreadPlugin from '../dev.perfetto.Thread';
import AsyncSlicesPlugin from '../dev.perfetto.AsyncSlices';

// SuspendResumeSliceTrack exists so as to override the `onSliceClick` function
// in AsyncSliceTrack.
// TODO(stevegolton): Remove this?
class SuspendResumeSliceTrack extends AsyncSliceTrack {
  constructor(
    args: NewTrackArgs,
    maxDepth: number,
    trackIds: number[],
    private readonly threads: ThreadMap,
  ) {
    super(args, maxDepth, trackIds);
  }

  onSliceClick(args: OnSliceClickArgs<Slice>) {
    this.trace.selection.selectTrackEvent(this.uri, args.slice.id);
  }

  override detailsPanel() {
    return new SuspendResumeDetailsPanel(this.trace, this.threads);
  }
}

export default class implements PerfettoPlugin {
  static readonly id = 'org.kernel.SuspendResumeLatency';
  static readonly dependencies = [ThreadPlugin, AsyncSlicesPlugin];

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
        where t.name = "Suspend/Resume Latency"
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
    const displayName = `Suspend/Resume Latency`;
    ctx.tracks.registerTrack({
      uri,
      title: displayName,
      tags: {
        trackIds,
        kind: SLICE_TRACK_KIND,
      },
      track: new SuspendResumeSliceTrack(
        {uri, trace: ctx},
        maxDepth,
        trackIds,
        threads,
      ),
    });

    // Display the track in the UI.
    const track = new TrackNode({uri, title: displayName});
    ctx.workspace.addChildInOrder(track);
  }
}
