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
import {
  CounterTrack,
  type CounterTrackAttrs,
} from '../../components/tracks/counter_track';
import type {TrackRenderContext} from '../../public/track';
import {Button} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import type {AudioPlayer} from './audio_player';

// Cycled by the track-shell speed button (the area-selection tab has the full
// dropdown).
const SHELL_RATES = [1, 1.5, 2, 0.5];

// The waveform counter track plus a play/stop button on the track shell, so the
// whole stream can be played with one click — no range selection needed. (To
// play just part of it, drag a range and use the area-selection Play button.)
export class AudioWaveformTrack extends CounterTrack {
  private readonly player: AudioPlayer;

  constructor(attrs: CounterTrackAttrs, player: AudioPlayer) {
    super(attrs);
    this.player = player;
  }

  render(trackCtx: TrackRenderContext): void {
    super.render(trackCtx);
    // This stream's own playhead, drawn on its own canvas — independent of any
    // other stream that's playing.
    const ts = this.player.currentTs();
    if (ts === undefined) return;
    const {ctx, size, timescale} = trackCtx;
    const x = Math.round(timescale.timeToPx(ts));
    if (x < 0 || x > size.width) return;
    ctx.save();
    ctx.strokeStyle = '#ff5252';
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, size.height);
    ctx.stroke();
    ctx.restore();
  }

  getTrackShellButtons(): m.Children {
    const p = this.player;
    if (!p.available) return undefined;
    return [
      m(Button, {
        icon: p.playing ? 'pause' : 'play_arrow',
        title: p.playing ? 'Pause' : p.paused ? 'Resume' : 'Play audio',
        intent: p.playing ? Intent.Warning : Intent.Primary,
        compact: true,
        onclick: (e: Event) => {
          // Don't let the click fall through to track selection.
          e.stopPropagation();
          if (p.playing) {
            p.pause();
          } else {
            void p.playAll();
          }
          m.redraw();
        },
      }),
      m(Button, {
        label: `${p.playbackRate}x`,
        title: 'Playback speed',
        compact: true,
        onclick: (e: Event) => {
          e.stopPropagation();
          const i = SHELL_RATES.indexOf(p.playbackRate);
          p.setRate(SHELL_RATES[(i + 1) % SHELL_RATES.length]);
        },
      }),
    ];
  }
}
