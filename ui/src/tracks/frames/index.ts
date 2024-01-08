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
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {getTrackName} from '../../public/utils';
import {
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';

import {ActualFramesTrack} from './actual_frames_track';
import {
  ActualFramesTrack as ActualFramesTrackV2,
} from './actual_frames_track_v2';
import {ExpectedFramesTrack} from './expected_frames_track';
import {
  ExpectedFramesTrack as ExpectedFramesTrackV2,
} from './expected_frames_track_v2';

export const EXPECTED_FRAMES_SLICE_TRACK_KIND = 'ExpectedFramesSliceTrack';
export const ACTUAL_FRAMES_SLICE_TRACK_KIND = 'ActualFramesSliceTrack';

class FramesPlugin implements Plugin {
  onActivate(_ctx: PluginContext): void {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    this.addExpectedFrames(ctx);
    this.addActualFrames(ctx);
  }

  async addExpectedFrames(ctx: PluginContextTrace): Promise<void> {
    const {engine} = ctx;
    const result = await engine.query(`
      with process_async_tracks as materialized (
        select
          process_track.upid as upid,
          process_track.name as trackName,
          process.name as processName,
          process.pid as pid,
          group_concat(process_track.id) as trackIds,
          count(1) as trackCount
        from process_track
        left join process using(upid)
        where process_track.name = "Expected Timeline"
        group by
          process_track.upid,
          process_track.name
      )
      select
        t.*,
        max_layout_depth(t.trackCount, t.trackIds) as maxDepth
      from process_async_tracks t;
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

      const displayName = getTrackName(
          {name: trackName, upid, pid, processName, kind: 'ExpectedFrames'});

      ctx.registerTrack({
        uri: `perfetto.ExpectedFrames#${upid}`,
        displayName,
        trackIds,
        kind: EXPECTED_FRAMES_SLICE_TRACK_KIND,
        track: ({trackKey}) => {
          return new ExpectedFramesTrack(
              engine,
              maxDepth,
              trackKey,
              trackIds,
          );
        },
      });

      ctx.registerTrack({
        uri: `perfetto.ExpectedFrames#${upid}.v2`,
        displayName,
        trackIds,
        kind: EXPECTED_FRAMES_SLICE_TRACK_KIND,
        track: ({trackKey}) => {
          return new ExpectedFramesTrackV2(
              engine,
              maxDepth,
              trackKey,
              trackIds,
          );
        },
      });
    }
  }

  async addActualFrames(ctx: PluginContextTrace): Promise<void> {
    const {engine} = ctx;
    const result = await engine.query(`
      with process_async_tracks as materialized (
        select
          process_track.upid as upid,
          process_track.name as trackName,
          process.name as processName,
          process.pid as pid,
          group_concat(process_track.id) as trackIds,
          count(1) as trackCount
        from process_track
        left join process using(upid)
        where process_track.name = "Actual Timeline"
        group by
          process_track.upid,
          process_track.name
      )
      select
        t.*,
        max_layout_depth(t.trackCount, t.trackIds) as maxDepth
      from process_async_tracks t;
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
      const displayName =
          getTrackName({name: trackName, upid, pid, processName, kind});

      ctx.registerTrack({
        uri: `perfetto.ActualFrames#${upid}`,
        displayName,
        trackIds,
        kind: ACTUAL_FRAMES_SLICE_TRACK_KIND,
        track: ({trackKey}) => {
          return new ActualFramesTrack(
              engine,
              maxDepth,
              trackKey,
              trackIds,
          );
        },
      });

      ctx.registerTrack({
        uri: `perfetto.ActualFrames#${upid}.v2`,
        displayName,
        trackIds,
        kind: ACTUAL_FRAMES_SLICE_TRACK_KIND,
        track: ({trackKey}) => {
          return new ActualFramesTrackV2(
              engine,
              maxDepth,
              trackKey,
              trackIds,
          );
        },
      });
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.Frames',
  plugin: FramesPlugin,
};
