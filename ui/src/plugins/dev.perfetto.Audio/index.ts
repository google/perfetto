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
import {AudioPlayer} from './audio_player';
import {AudioSelectionTab} from './audio_selection_tab';
import {AudioWaveformTrack} from './audio_waveform_track';

interface StreamInfo {
  streamId: number;
  streamName: string;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Audio';
  static readonly description =
    'Shows audio frames captured by the android.audio data source as a ' +
    'per-stream amplitude (waveform) counter track. Press play on the track ' +
    'to hear the whole stream, or select a range to play just that part ' +
    '(decoded via WebAudio).';

  async onTraceLoad(ctx: Trace): Promise<void> {
    const res = await ctx.engine.query(`
      SELECT stream_id AS streamId, MAX(stream_name) AS streamName
      FROM __intrinsic_audio_frames
      GROUP BY stream_id
      ORDER BY stream_id
    `);
    const streams: StreamInfo[] = [];
    const it = res.iter({streamId: NUM, streamName: STR_NULL});
    for (; it.valid(); it.next()) {
      streams.push({
        streamId: it.streamId,
        streamName: it.streamName ?? `Stream ${it.streamId}`,
      });
    }
    if (streams.length === 0) return;

    const group = new TrackNode({
      name: 'Audio',
      isSummary: true,
      sortOrder: -54,
    });

    for (const stream of streams) {
      const uri = `/audio/${stream.streamId}`;
      const player = new AudioPlayer(ctx, stream.streamId);

      // The waveform: a counter track whose value is the per-frame peak
      // amplitude, so it reads as the amplitude envelope over time. The
      // subclass adds a play/stop button to the track shell.
      ctx.tracks.registerTrack({
        uri,
        renderer: new AudioWaveformTrack(
          {
            trace: ctx,
            uri,
            sqlSource: `
              SELECT ts, peak AS value
              FROM __intrinsic_audio_frames
              WHERE stream_id = ${stream.streamId} AND is_config IS NULL
            `,
            unit: 'amp',
            yOverrideMinimum: 0,
            yOverrideMaximum: 1000,
          },
          player,
        ),
      });
      group.addChildInOrder(new TrackNode({uri, name: stream.streamName}));

      ctx.selection.registerAreaSelectionTab(
        new AudioSelectionTab(
          ctx,
          uri,
          stream.streamId,
          stream.streamName,
          player,
        ),
      );
    }

    ctx.defaultWorkspace.addChildInOrder(group);
  }
}
