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

import './video_frames.scss';
import m from 'mithril';
import {Icons} from '../../base/semantic_icons';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {NUM, STR_NULL} from '../../trace_processor/query_result';
import {Button} from '../../widgets/button';
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

    const group = new TrackNode({
      name: 'Video Frames',
      isSummary: true,
      sortOrder: -55,
    });

    const players = new Map<string, VideoFramePlayer>();
    for (const stream of streams) {
      const uri = `/video_frames/${stream.displayId}`;
      const player = new VideoFramePlayer(
        ctx,
        uri,
        stream.displayId,
        stream.displayName,
      );
      players.set(uri, player);

      ctx.tracks.registerTrack({
        uri,
        renderer: createVideoFramesTrack(ctx, uri, stream.displayId, player),
      });
      group.addChildInOrder(new TrackNode({uri, name: stream.displayName}));
    }

    ctx.defaultWorkspace.addChildInOrder(group);

    // Select a time range over a video track to download just that region.
    ctx.selection.registerAreaSelectionTab({
      id: 'dev.perfetto.VideoFrames#region',
      name: 'Video',
      render(selection) {
        // A selection may span one video track per display; download each.
        const selected = selection.trackUris
          .filter((u) => players.has(u))
          .map((u) => players.get(u)!);
        if (selected.length === 0) return undefined;
        return {
          isLoading: false,
          content: m(
            '.pf-video-region-download',
            'Download the selected time range as an .mp4 per display.',
          ),
          buttons: m(Button, {
            icon: Icons.Download,
            label: 'Download region (.mp4)',
            onclick: () => {
              for (const player of selected) {
                void player.downloadRegion(selection.start, selection.end);
              }
            },
          }),
        };
      },
    });

    // The per-video panel downloads its own stream; this does every stream.
    ctx.commands.registerCommand({
      id: 'dev.perfetto.VideoFrames#DownloadAll',
      name: 'Download all display videos',
      callback: async () => {
        for (const player of players.values()) {
          await player.downloadVideo();
        }
      },
    });
  }
}
