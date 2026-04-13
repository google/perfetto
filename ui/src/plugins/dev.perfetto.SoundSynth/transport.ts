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

interface TransportAttrs {
  rendering: boolean;
  wavData: ArrayBuffer | null;
  onRender: () => void;
  autoPlay?: boolean;
  onPlaybackStarted?: () => void;
}

export class Transport implements m.ClassComponent<TransportAttrs> {
  private audioCtx: AudioContext | null = null;
  private sourceNode: AudioBufferSourceNode | null = null;
  private playing = false;
  private lastAutoPlayedBuf: ArrayBuffer | null = null;

  view(vnode: m.Vnode<TransportAttrs>) {
    const {rendering, wavData, onRender, autoPlay, onPlaybackStarted} =
      vnode.attrs;

    // Auto-play when flagged and a new buffer arrives.
    if (autoPlay && wavData && wavData !== this.lastAutoPlayedBuf) {
      this.lastAutoPlayedBuf = wavData;
      // Defer the async playback out of the render cycle.
      void this.startPlayback(wavData).then(() => {
        if (onPlaybackStarted) onPlaybackStarted();
      });
    }

    return m(
      '.transport-bar',
      {
        style: {
          display: 'flex',
          alignItems: 'center',
          padding: '8px 12px',
          borderTop: '1px solid #ddd',
          gap: '12px',
          background: '#fafafa',
        },
      },
      m(
        'button',
        {
          style: {
            padding: '6px 20px',
            background: rendering ? '#ccc' : '#4285f4',
            color: 'white',
            border: 'none',
            borderRadius: '4px',
            cursor: rendering ? 'wait' : 'pointer',
            fontWeight: 'bold',
          },
          disabled: rendering,
          onclick: onRender,
        },
        rendering ? 'Rendering...' : '\u25B6 Render',
      ),
      wavData
        ? m(
            'button',
            {
              style: {
                padding: '6px 16px',
                border: '1px solid #ccc',
                borderRadius: '4px',
                cursor: 'pointer',
                background: this.playing ? '#e8eaf6' : 'white',
              },
              onclick: () => this.togglePlayback(wavData),
            },
            this.playing ? '\u23F9 Stop' : '\u25B6 Play',
          )
        : null,
      wavData
        ? m('span', {style: {fontSize: '12px', color: '#666'}},
            `${(wavData.byteLength / 1024).toFixed(0)} KB`)
        : null,
      !wavData && !rendering
        ? m('span', {style: {fontSize: '12px', color: '#999'}},
            'Click Render to synthesize audio from the trace')
        : null,
    );
  }

  private async togglePlayback(wavData: ArrayBuffer) {
    if (this.playing) {
      this.stopPlayback();
      return;
    }
    await this.startPlayback(wavData);
  }

  private async startPlayback(wavData: ArrayBuffer) {
    try {
      this.stopPlayback();
      if (!this.audioCtx) {
        this.audioCtx = new AudioContext({sampleRate: 48000});
      }
      const audioBuffer = await this.audioCtx.decodeAudioData(
        wavData.slice(0),
      );
      this.sourceNode = this.audioCtx.createBufferSource();
      this.sourceNode.buffer = audioBuffer;
      this.sourceNode.connect(this.audioCtx.destination);
      this.sourceNode.onended = () => {
        this.playing = false;
        this.sourceNode = null;
        m.redraw();
      };
      this.sourceNode.start();
      this.playing = true;
      m.redraw();
    } catch (e) {
      console.error('Playback error:', e);
      this.playing = false;
      m.redraw();
    }
  }

  private stopPlayback() {
    if (this.sourceNode) {
      this.sourceNode.stop();
      this.sourceNode = null;
    }
    this.playing = false;
    m.redraw();
  }

  onremove() {
    this.stopPlayback();
    if (this.audioCtx) {
      this.audioCtx.close();
      this.audioCtx = null;
    }
  }
}
