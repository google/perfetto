// Copyright (C) 2026 The Android Open Source Project
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
import {NUM, STR} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {createVideoFramesTrack} from './video_frames_track';
import {VideoFramesSelectionTab} from './video_frames_selection_tab';
import {VideoFramePlayer} from './playback_state';

interface StreamInfo {
  trackId: number;
  trackName: string;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.VideoFrames';

  async onTraceLoad(ctx: Trace): Promise<void> {
    await ctx.engine.query('INCLUDE PERFETTO MODULE android.video_frames');

    const res = await ctx.engine.query(`
      SELECT
        COALESCE(track_id, 0) AS trackId,
        COALESCE(track_name, 'Video Frames') AS trackName
      FROM android_video_frames
      GROUP BY trackId
      ORDER BY trackId
    `);

    const streams: StreamInfo[] = [];
    const it = res.iter({trackId: NUM, trackName: STR});
    for (; it.valid(); it.next()) {
      streams.push({trackId: it.trackId, trackName: it.trackName});
    }
    if (streams.length === 0) return;

    const group = new TrackNode({
      name: 'Video Frames',
      isSummary: true,
      sortOrder: -55,
    });

    for (const stream of streams) {
      const uri = `/video_frames/${stream.trackId}`;
      const viewName = `_video_frames_track_${stream.trackId}`;

      await ctx.engine.query(`
        CREATE OR REPLACE PERFETTO VIEW ${viewName} AS
        SELECT
          id,
          ts,
          0 AS dur,
          'Frame ' || frame_number AS name
        FROM android_video_frames
        WHERE COALESCE(track_id, 0) = ${stream.trackId}
      `);

      const player = new VideoFramePlayer(ctx, uri, stream.trackId);

      ctx.tracks.registerTrack({
        uri,
        renderer: createVideoFramesTrack(ctx, uri, viewName, player),
      });

      group.addChildInOrder(new TrackNode({uri, name: stream.trackName}));

      ctx.selection.registerAreaSelectionTab(
        new VideoFramesSelectionTab(ctx, uri, stream.trackId, stream.trackName),
      );
    }

    ctx.defaultWorkspace.addChildInOrder(group);
  }
}
