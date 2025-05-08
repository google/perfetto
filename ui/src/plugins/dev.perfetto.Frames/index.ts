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

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {TrackNode} from '../../public/workspace';
import {NUM, STR} from '../../trace_processor/query_result';
import {createActualFramesTrack} from './actual_frames_track';
import {createExpectedFramesTrack} from './expected_frames_track';
import {
  ACTUAL_FRAMES_SLICE_TRACK_KIND,
  FrameSelectionAggregator,
} from './frame_selection_aggregator';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {createAggregationToTabAdaptor} from '../../components/aggregation_adapter';

// Build a standardized URI for a frames track
function makeUri(upid: number, kind: 'expected_frames' | 'actual_frames') {
  return `/process_${upid}/${kind}`;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Frames';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    this.addExpectedFrames(ctx);
    this.addActualFrames(ctx);
    ctx.selection.registerAreaSelectionTab(
      createAggregationToTabAdaptor(ctx, new FrameSelectionAggregator()),
    );
  }

  async addExpectedFrames(ctx: Trace): Promise<void> {
    const {engine} = ctx;
    const result = await engine.query(`
      with summary as (
        select
          pt.upid,
          group_concat(id) AS track_ids,
          count() AS track_count
        from process_track pt
        join _slice_track_summary USING (id)
        where pt.type = 'android_expected_frame_timeline'
        group by pt.upid
      )
      select
        t.upid,
        t.track_ids as trackIds,
        __max_layout_depth(t.track_count, t.track_ids) as maxDepth
      from summary t
    `);

    const it = result.iter({
      upid: NUM,
      trackIds: STR,
      maxDepth: NUM,
    });

    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const maxDepth = it.maxDepth;

      const title = 'Expected Timeline';
      const uri = makeUri(upid, 'expected_frames');
      ctx.tracks.registerTrack({
        uri,
        title,
        track: createExpectedFramesTrack(ctx, uri, maxDepth, trackIds),
        tags: {
          trackIds,
          upid,
        },
      });
      const group = ctx.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForProcess(upid);
      const track = new TrackNode({uri, title, sortOrder: -50});
      group?.addChildInOrder(track);
    }
  }

  async addActualFrames(ctx: Trace): Promise<void> {
    const {engine} = ctx;
    const result = await engine.query(`
      with summary as (
        select
          pt.upid,
          group_concat(id) AS track_ids,
          count() AS track_count
        from process_track pt
        join _slice_track_summary USING (id)
        where pt.type = 'android_actual_frame_timeline'
        group by pt.upid
      )
      select
        t.upid,
        t.track_ids as trackIds,
        __max_layout_depth(t.track_count, t.track_ids) as maxDepth
      from summary t
    `);

    const it = result.iter({
      upid: NUM,
      trackIds: STR,
      maxDepth: NUM,
    });
    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const maxDepth = it.maxDepth;

      const title = 'Actual Timeline';
      const uri = makeUri(upid, 'actual_frames');
      ctx.tracks.registerTrack({
        uri,
        title,
        track: createActualFramesTrack(ctx, uri, maxDepth, trackIds),
        tags: {
          upid,
          trackIds,
          kind: ACTUAL_FRAMES_SLICE_TRACK_KIND,
        },
      });
      const group = ctx.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForProcess(upid);
      const track = new TrackNode({uri, title, sortOrder: -50});
      group?.addChildInOrder(track);
    }
  }
}
