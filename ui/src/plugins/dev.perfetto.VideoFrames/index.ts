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

import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {NUM, STR_NULL} from '../../trace_processor/query_result';
import {createTouchesTrack} from './touches_track';
import {queryTouchDisplays, TouchOverlay} from './touch_overlay';
import {VideoFrameInputSection} from './video_frame_input_section';
import {VideoFramePlayer} from './video_frame_player';
import {createVideoFramesTrack} from './video_frames_track';

interface StreamInfo {
  displayId: number;
  displayName: string;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.VideoFrames';
  static readonly description =
    'Shows display frames captured by the android.display.video data source. ' +
    'Adds a per-display timeline track with a decoded frame preview and ' +
    'playback.';

  async onTraceLoad(ctx: Trace): Promise<void> {
    const res = await ctx.engine.query(`
      SELECT display_id AS displayId, MAX(display_name) AS displayName
      FROM __intrinsic_video_frames
      GROUP BY display_id
      ORDER BY display_id
    `);

    const streams: StreamInfo[] = [];
    const it = res.iter({displayId: NUM, displayName: STR_NULL});
    for (; it.valid(); it.next()) {
      streams.push({
        displayId: it.displayId,
        displayName: it.displayName ?? `Display ${it.displayId}`,
      });
    }
    if (streams.length === 0) return;

    const touchDisplays = await queryTouchDisplays(ctx.engine);

    const group = new TrackNode({
      name: 'Video Frames',
      isSummary: true,
      sortOrder: -55,
    });

    for (const stream of streams) {
      const uri = `/video_frames/${stream.displayId}`;
      const player = new VideoFramePlayer(ctx, uri, stream.displayId);
      const hasTouches = touchDisplays.has(stream.displayId);
      const touches = hasTouches
        ? new TouchOverlay(ctx, player, stream.displayId)
        : undefined;
      const inputSection = hasTouches
        ? new VideoFrameInputSection(ctx, uri, stream.displayId)
        : undefined;

      ctx.tracks.registerTrack({
        uri,
        renderer: createVideoFramesTrack(
          ctx,
          uri,
          stream.displayId,
          player,
          touches,
          inputSection,
        ),
      });
      group.addChildInOrder(new TrackNode({uri, name: stream.displayName}));

      if (hasTouches) {
        const touchesUri = `${uri}/touches`;
        ctx.tracks.registerTrack({
          uri: touchesUri,
          renderer: createTouchesTrack(ctx, touchesUri, stream.displayId),
        });
        const name =
          streams.length === 1 ? 'Touches' : `${stream.displayName} touches`;
        group.addChildInOrder(new TrackNode({uri: touchesUri, name}));
      }
    }

    ctx.defaultWorkspace.addChildInOrder(group);
  }
}
