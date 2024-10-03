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

import {
  ACTUAL_FRAMES_SLICE_TRACK_KIND,
  EXPECTED_FRAMES_SLICE_TRACK_KIND,
} from '../../public/track_kinds';
import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {getOrCreateGroupForProcess} from '../../public/standard_groups';
import {getTrackName} from '../../public/utils';
import {TrackNode} from '../../public/workspace';
import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';
import {ActualFramesTrack} from './actual_frames_track';
import {ExpectedFramesTrack} from './expected_frames_track';
import {FrameSelectionAggregator} from './frame_selection_aggregator';
import {ThreadSliceDetailsPanel} from '../../frontend/thread_slice_details_tab';

class FramesPlugin implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    this.addExpectedFrames(ctx);
    this.addActualFrames(ctx);
    ctx.selection.registerAreaSelectionAggreagtor(
      new FrameSelectionAggregator(),
    );
  }

  async addExpectedFrames(ctx: Trace): Promise<void> {
    const {engine} = ctx;
    const result = await engine.query(`
      select
        upid,
        t.name as trackName,
        t.track_ids as trackIds,
        process.name as processName,
        process.pid as pid,
        __max_layout_depth(t.track_count, t.track_ids) as maxDepth
      from _process_track_summary_by_upid_and_parent_id_and_name t
      join process using(upid)
      where t.name = "Expected Timeline"
    `);

    const it = result.iter({
      upid: NUM,
      trackName: STR_NULL,
      trackIds: STR,
      processName: STR_NULL,
      pid: NUM_NULL,
      maxDepth: NUM,
    });

    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const trackName = it.trackName;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const processName = it.processName;
      const pid = it.pid;
      const maxDepth = it.maxDepth;

      const title = getTrackName({
        name: trackName,
        upid,
        pid,
        processName,
        kind: 'ExpectedFrames',
      });

      const uri = `/process_${upid}/expected_frames`;
      ctx.tracks.registerTrack({
        uri,
        title,
        track: new ExpectedFramesTrack(ctx, maxDepth, uri, trackIds),
        tags: {
          trackIds,
          upid,
          kind: EXPECTED_FRAMES_SLICE_TRACK_KIND,
        },
        detailsPanel: () => new ThreadSliceDetailsPanel(ctx, 'slice'),
      });
      const group = getOrCreateGroupForProcess(ctx.workspace, upid);
      const track = new TrackNode({uri, title, sortOrder: -50});
      group.addChildInOrder(track);
    }
  }

  async addActualFrames(ctx: Trace): Promise<void> {
    const {engine} = ctx;
    const result = await engine.query(`
      select
        upid,
        t.name as trackName,
        t.track_ids as trackIds,
        process.name as processName,
        process.pid as pid,
        __max_layout_depth(t.track_count, t.track_ids) as maxDepth
      from _process_track_summary_by_upid_and_parent_id_and_name t
      join process using(upid)
      where t.name = "Actual Timeline"
    `);

    const it = result.iter({
      upid: NUM,
      trackName: STR_NULL,
      trackIds: STR,
      processName: STR_NULL,
      pid: NUM_NULL,
      maxDepth: NUM_NULL,
    });
    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const trackName = it.trackName;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const processName = it.processName;
      const pid = it.pid;
      const maxDepth = it.maxDepth;

      if (maxDepth === null) {
        // If there are no slices in this track, skip it.
        continue;
      }

      const kind = 'ActualFrames';
      const title = getTrackName({
        name: trackName,
        upid,
        pid,
        processName,
        kind,
      });

      const uri = `/process_${upid}/actual_frames`;
      ctx.tracks.registerTrack({
        uri,
        title,
        track: new ActualFramesTrack(ctx, maxDepth, uri, trackIds),
        tags: {
          upid,
          trackIds,
          kind: ACTUAL_FRAMES_SLICE_TRACK_KIND,
        },
        detailsPanel: () => new ThreadSliceDetailsPanel(ctx, 'slice'),
      });
      const group = getOrCreateGroupForProcess(ctx.workspace, upid);
      const track = new TrackNode({uri, title, sortOrder: -50});
      group.addChildInOrder(track);
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.Frames',
  plugin: FramesPlugin,
};
