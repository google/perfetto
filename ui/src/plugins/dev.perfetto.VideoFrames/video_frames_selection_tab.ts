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

import m from 'mithril';
import {Time} from '../../base/time';
import {Timestamp} from '../../components/widgets/timestamp';
import type {Trace} from '../../public/trace';
import type {
  AreaSelection,
  AreaSelectionTab,
  ContentWithLoadingFlag,
} from '../../public/selection';
import {NUM} from '../../trace_processor/query_result';
import {Button, ButtonBar} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import type {VideoFramePlayer} from './video_frame_player';

// Area-selection tab: filters the player's frames to a ts range and drives
// prev/play/next through that subset. All decoding/painting lives on
// VideoFramePlayer.
export class VideoFramesSelectionTab implements AreaSelectionTab {
  readonly id: string;
  readonly name: string;
  readonly priority = 10;

  private readonly trace: Trace;
  private readonly trackUri: string;
  private readonly displayId: number;
  private readonly player: VideoFramePlayer;

  private rangeIds: number[] = [];
  private idx = 0;
  private loading = false;
  private lastSelectionKey = '';

  constructor(
    trace: Trace,
    trackUri: string,
    displayId: number,
    displayName: string,
    player: VideoFramePlayer,
  ) {
    this.trace = trace;
    this.trackUri = trackUri;
    this.displayId = displayId;
    this.id = `video_frames_playback_${displayId}`;
    this.name = displayName;
    this.player = player;
  }

  render(selection: AreaSelection): ContentWithLoadingFlag | undefined {
    if (!selection.trackUris.includes(this.trackUri)) return undefined;

    const key = `${selection.start}-${selection.end}`;
    if (key !== this.lastSelectionKey) {
      this.lastSelectionKey = key;
      this.player.stop();
      void this.loadRange(selection);
    }
    if (this.rangeIds.length === 0 && !this.loading) return undefined;

    const total = this.rangeIds.length;
    const frame = this.player.currentFrame;
    const p = this.player;

    return {
      isLoading: this.loading,
      buttons: m(
        ButtonBar,
        m(Button, {
          icon: 'skip_previous',
          compact: true,
          disabled: this.idx <= 0 || p.playing,
          onclick: () => void this.seekTo(this.idx - 1),
        }),
        m(Button, {
          icon: p.playing ? 'pause' : 'play_arrow',
          intent: p.playing ? Intent.Warning : Intent.Primary,
          compact: true,
          onclick: () => p.togglePlay(),
        }),
        m(Button, {
          icon: 'skip_next',
          compact: true,
          disabled: this.idx >= total - 1 || p.playing,
          onclick: () => void this.seekTo(this.idx + 1),
        }),
      ),
      content: m(
        GridLayout,
        m(
          Section,
          {title: 'Details'},
          frame !== undefined
            ? m(
                Tree,
                m(TreeNode, {
                  left: 'Frame',
                  right: `${this.idx + 1} of ${total}`,
                }),
                m(TreeNode, {
                  left: 'Timestamp',
                  right: m(Timestamp, {
                    trace: this.trace,
                    ts: Time.fromRaw(frame.ts),
                  }),
                }),
              )
            : m('span', 'Loading...'),
        ),
        m(
          Section,
          {title: 'Preview'},
          m('canvas.pf-video-frame-preview', {
            oncreate: ({dom}) => p.attachCanvas(dom as HTMLCanvasElement),
            onremove: () => p.detachCanvas(),
          }),
        ),
      ),
    };
  }

  private async loadRange(selection: AreaSelection): Promise<void> {
    this.loading = true;
    this.rangeIds = [];
    this.idx = 0;
    m.redraw();
    const res = await this.trace.engine.query(`
      SELECT id
      FROM android_video_frames
      WHERE display_id = ${this.displayId}
        AND COALESCE(is_config, 0) = 0
        AND ts >= ${selection.start} AND ts <= ${selection.end}
      ORDER BY ts
    `);
    const it = res.iter({id: NUM});
    for (; it.valid(); it.next()) this.rangeIds.push(it.id);
    this.loading = false;
    if (this.rangeIds.length > 0) await this.seekTo(0);
    m.redraw();
  }

  private async seekTo(idx: number): Promise<void> {
    if (idx < 0 || idx >= this.rangeIds.length) return;
    this.idx = idx;
    await this.player.ensureFramesLoaded();
    await this.player.seek(this.rangeIds[idx]);
  }
}
