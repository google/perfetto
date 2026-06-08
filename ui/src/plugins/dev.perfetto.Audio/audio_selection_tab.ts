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
import type {
  AreaSelection,
  AreaSelectionTab,
  ContentWithLoadingFlag,
} from '../../public/selection';
import type {Trace} from '../../public/trace';
import {Button, ButtonBar} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {Select} from '../../widgets/select';
import type {AudioPlayer} from './audio_player';

const PLAYBACK_RATES = [0.5, 0.75, 1, 1.5, 2];

// Plays the audio of the dragged time range. Selecting a span on the waveform
// counter track highlights it (the area selection) and this tab plays exactly
// that range.
export class AudioSelectionTab implements AreaSelectionTab {
  readonly id: string;
  readonly name: string;
  readonly priority = 10;

  private readonly trackUri: string;
  private readonly player: AudioPlayer;
  private selection?: AreaSelection;

  constructor(
    _trace: Trace,
    trackUri: string,
    streamId: number,
    displayName: string,
    player: AudioPlayer,
  ) {
    this.trackUri = trackUri;
    this.player = player;
    this.id = `audio_playback_${streamId}`;
    this.name = displayName;
  }

  render(selection: AreaSelection): ContentWithLoadingFlag | undefined {
    if (!selection.trackUris.includes(this.trackUri)) return undefined;
    this.selection = selection;
    const p = this.player;
    return {
      isLoading: false,
      buttons: m(
        ButtonBar,
        m(Button, {
          icon: p.playing ? 'pause' : 'play_arrow',
          label: p.playing ? 'Pause' : p.paused ? 'Resume' : 'Play selection',
          intent: p.playing ? Intent.Warning : Intent.Primary,
          compact: true,
          disabled: !p.available,
          onclick: () => {
            if (p.playing) {
              p.pause();
            } else if (this.selection) {
              void p.playRange(this.selection.start, this.selection.end);
            }
            m.redraw();
          },
        }),
        p.available &&
          m(
            Select,
            {
              title: 'Playback speed',
              onchange: (e: Event) =>
                p.setRate(parseFloat((e.target as HTMLSelectElement).value)),
            },
            PLAYBACK_RATES.map((r) =>
              m('option', {value: r, selected: p.playbackRate === r}, `${r}x`),
            ),
          ),
        !p.available &&
          m(
            'span',
            'Playback requires WebAudio, unavailable in this browser or ' +
              'context.',
          ),
      ),
      content: m('span', 'Drag a range on the waveform, then press play.'),
    };
  }
}
