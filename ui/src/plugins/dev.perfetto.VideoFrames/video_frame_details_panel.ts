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
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventSelection} from '../../public/selection';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {Button, ButtonBar} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {Select} from '../../widgets/select';
import {Timestamp} from '../../components/widgets/timestamp';
import {Time} from '../../base/time';
import {VideoFramePlayer, FPS_OPTIONS} from './playback_state';

export class VideoFrameDetailsPanel implements TrackEventDetailsPanel {
  private readonly player: VideoFramePlayer;

  constructor(player: VideoFramePlayer) {
    this.player = player;
  }

  async load(sel: TrackEventSelection) {
    // If playback is driving the selection, the player already has the
    // right frame loaded — nothing to do.
    if (this.player.playing) return;

    await this.player.ensureFramesLoaded();
    await this.player.goToId(sel.eventId);
  }

  render() {
    const p = this.player;
    const frame = p.currentFrame;
    if (!frame) {
      return m(DetailsShell, {title: 'Video Frame'}, m('span', 'Loading...'));
    }

    return m(
      DetailsShell,
      {
        title: 'Video Frame',
        description: `Frame ${frame.frameNumber}`,
        buttons: this.renderControls(),
      },
      m(
        GridLayout,
        m(
          Section,
          {title: 'Details'},
          m(
            Tree,
            m(TreeNode, {
              left: 'Frame number',
              right: `${frame.frameNumber}`,
            }),
            m(TreeNode, {
              left: 'Timestamp',
              right: m(Timestamp, {
                trace: p.trace,
                ts: Time.fromRaw(frame.ts),
              }),
            }),
          ),
        ),
        m(
          Section,
          {title: 'Preview'},
          p.imageUrl
            ? m('img.pf-video-frame-preview', {src: p.imageUrl})
            : m('span', 'No image data'),
        ),
      ),
    );
  }

  private renderControls(): m.Children {
    const p = this.player;
    const idx = p.currentIdx;
    const total = p.frames.length;

    return m(ButtonBar, [
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
          value: String(p.fps),
          onchange: (e: Event) => {
            p.setFps(Number((e.target as HTMLSelectElement).value));
          },
        },
        FPS_OPTIONS.map((f) =>
          m('option', {value: String(f), selected: f === p.fps}, `${f} fps`),
        ),
      ),
    ]);
  }
}
