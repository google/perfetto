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
import {ensureIsInstance} from '../../base/assert';
import {Time} from '../../base/time';
import {Timestamp} from '../../components/widgets/timestamp';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {TrackEventSelection} from '../../public/selection';
import {Button, ButtonBar} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Select} from '../../widgets/select';
import {Tree, TreeNode} from '../../widgets/tree';
import type {VideoFramePlayer} from './video_frame_player';

// Playback speed options, in real-time multiples.
const PLAYBACK_RATES = [0.1, 0.2, 0.5, 1, 1.5, 2];

export class VideoFrameDetailsPanel implements TrackEventDetailsPanel {
  private readonly player: VideoFramePlayer;

  constructor(player: VideoFramePlayer) {
    this.player = player;
  }

  async load(sel: TrackEventSelection) {
    // Skip re-decode when the selection update was triggered by us
    // (playback loop, next/prev, or the player's own selectAndSeek path).
    // The player already has currentIdx pointing at this event.
    if (this.player.playing) return;
    await this.player.ensureFramesLoaded();
    if (this.player.frames[this.player.currentIdx]?.id === sel.eventId) return;
    // Don't await: while load() is pending the panel sits in its loading
    // state, which remounts and blanks the <canvas>. seek() cancels stale
    // decodes itself.
    this.player.seek(sel.eventId);
  }

  render() {
    const p = this.player;
    const frame = p.currentFrame;
    if (frame === undefined) {
      return m(DetailsShell, {title: 'Video Frame'}, m('span', 'Loading...'));
    }
    const detailRows = [
      m(TreeNode, {left: 'Frame number', right: `${frame.frameNumber}`}),
      m(TreeNode, {
        left: 'Timestamp',
        right: m(Timestamp, {trace: p.trace, ts: Time.fromRaw(frame.ts)}),
      }),
    ];
    for (const err of p.errors) {
      detailRows.push(m(TreeNode, {left: 'Stream error', right: err}));
    }
    return m(
      DetailsShell,
      {
        title: 'Video Frame',
        description: `Frame ${frame.frameNumber}`,
        buttons: this.renderControls(),
        className: 'pf-video-frame-shell',
      },
      m(
        GridLayout,
        m(Section, {title: 'Details'}, m(Tree, detailRows)),
        m(
          Section,
          {title: 'Preview', className: 'pf-video-frame-preview-section'},
          p.webCodecsAvailable
            ? m('canvas.pf-video-frame-preview', {
                oncreate: ({dom}) =>
                  p.attachCanvas(ensureIsInstance(dom, HTMLCanvasElement)),
                onremove: () => p.detachCanvas(),
              })
            : m(
                'span',
                'Frame preview requires WebCodecs, which is unavailable in ' +
                  'this browser or context (a secure context / https is ' +
                  'needed).',
              ),
        ),
      ),
    );
  }

  private renderControls(): m.Children {
    const p = this.player;
    const idx = p.currentIdx;
    const total = p.frames.length;
    return m(
      ButtonBar,
      m(Button, {
        icon: 'skip_previous',
        compact: true,
        disabled: idx <= 0 || p.playing,
        onclick: () => p.prev(),
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
        disabled: idx >= total - 1 || p.playing,
        onclick: () => p.next(),
      }),
      m(
        Select,
        {
          title: 'Playback speed',
          onchange: (e: Event) => {
            p.setPlaybackRate(Number((e.target as HTMLSelectElement).value));
          },
        },
        PLAYBACK_RATES.map((rate) =>
          m(
            'option',
            {value: rate, selected: rate === p.playbackRate},
            `${rate}x`,
          ),
        ),
      ),
      m(Button, {
        icon: 'photo_camera',
        title: 'Download this frame as a PNG',
        compact: true,
        disabled: !p.webCodecsAvailable,
        onclick: () => void p.downloadFrameImage(),
      }),
      m(Button, {
        icon: 'movie',
        title:
          'Download the whole video for this display as an .mp4 ' +
          '(select a time range on the track to download just that part)',
        compact: true,
        onclick: () => void p.downloadVideo(),
      }),
    );
  }
}
