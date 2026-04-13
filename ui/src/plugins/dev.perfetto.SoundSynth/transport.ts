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

// Transport bar: render button + single-stream audio playback.
//
// Invariant: at most one audio stream is playing at any time. The active
// source node is tracked by `sourceNode`, and every new startPlayback()
// call first stops whatever was playing. A generation counter defends
// against concurrent async calls (e.g. a second Test click during the
// first click's decodeAudioData await).

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
  // Incremented on every start/stop call. Used to cancel in-flight
  // decodeAudioData awaits when a newer playback is requested.
  private playbackGeneration = 0;

  view(vnode: m.Vnode<TransportAttrs>) {
    const {rendering, wavData, onRender, autoPlay, onPlaybackStarted} =
      vnode.attrs;

    // Stop any previous playback when the render starts (wavData=null)
    // or when a fresh buffer arrives. This guarantees there's never more
    // than one stream playing.
    if (wavData === null && this.playing) {
      this.stopPlayback();
    }

    // Auto-play when flagged and a new buffer arrives.
    if (autoPlay && wavData && wavData !== this.lastAutoPlayedBuf) {
      this.lastAutoPlayedBuf = wavData;
      // Defer the async playback out of the render cycle.
      void this.startPlayback(wavData).then(() => {
        if (onPlaybackStarted) onPlaybackStarted();
      });
    }

    // The Play/Stop button stays visible while something is playing,
    // even if wavData was cleared (e.g. during a re-render), so the
    // user always has a way to stop the current sound.
    const showPlayButton = wavData !== null || this.playing;
    const playButtonData = wavData ?? null;

    return m('.transport-bar', {
      style: {
        display: 'flex',
        alignItems: 'center',
        padding: '8px 12px',
        borderTop: '1px solid #ddd',
        gap: '12px',
        background: '#fafafa',
      },
    },
      m('button', {
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
      }, rendering ? 'Rendering...' : '\u25B6 Render'),
      showPlayButton
        ? m('button', {
            style: {
              padding: '6px 16px',
              border: '1px solid #ccc',
              borderRadius: '4px',
              cursor: 'pointer',
              background: this.playing ? '#e8eaf6' : 'white',
            },
            onclick: () => {
              if (this.playing) {
                this.stopPlayback();
              } else if (playButtonData) {
                void this.startPlayback(playButtonData);
              }
            },
          }, this.playing ? '\u23F9 Stop' : '\u25B6 Play')
        : null,
      wavData
        ? m('span', {style: {fontSize: '12px', color: '#666'}},
            `${(wavData.byteLength / 1024).toFixed(0)} KB`)
        : null,
      !wavData && !rendering && !this.playing
        ? m('span', {style: {fontSize: '12px', color: '#999'}},
            'Click Render to synthesize audio from the trace')
        : null,
    );
  }

  private async startPlayback(wavData: ArrayBuffer) {
    // Stop whatever was playing before and claim a fresh generation.
    // Any in-flight startPlayback awaits will see gen != playbackGeneration
    // and silently bail out.
    this.stopPlayback();
    const gen = ++this.playbackGeneration;

    try {
      if (!this.audioCtx) {
        this.audioCtx = new AudioContext({sampleRate: 48000});
      }
      const audioBuffer = await this.audioCtx.decodeAudioData(
        wavData.slice(0));

      // Check if a newer playback superseded this one during the await.
      if (gen !== this.playbackGeneration) return;

      // Also bail if our source was stopped by a stopPlayback call that
      // happened during the await.
      if (this.sourceNode !== null) {
        // Something else wrote to sourceNode? Shouldn't happen given
        // the generation check above, but be defensive.
        return;
      }

      const node = this.audioCtx.createBufferSource();
      node.buffer = audioBuffer;
      node.connect(this.audioCtx.destination);
      node.onended = () => {
        // Only clear state if this is still the active node. If the
        // playback was superseded, a newer node may have already been
        // stored in this.sourceNode.
        if (this.sourceNode === node) {
          this.playing = false;
          this.sourceNode = null;
          m.redraw();
        }
      };
      this.sourceNode = node;
      node.start();
      this.playing = true;
      m.redraw();
    } catch (e) {
      console.error('Playback error:', e);
      if (gen === this.playbackGeneration) {
        this.playing = false;
      }
      m.redraw();
    }
  }

  private stopPlayback() {
    // Bump the generation so any in-flight decode awaits bail out
    // when they resume.
    this.playbackGeneration++;
    if (this.sourceNode) {
      try {
        this.sourceNode.onended = null;
        this.sourceNode.stop();
      } catch (e) {
        // OK if already stopped.
      }
      try {
        this.sourceNode.disconnect();
      } catch (e) {
        // OK if already disconnected.
      }
      this.sourceNode = null;
    }
    this.playing = false;
    // Reset lastAutoPlayedBuf so if the same wavData arrives again
    // later, autoPlay can re-trigger.
    // (We intentionally keep lastAutoPlayedBuf here so we don't
    // auto-restart what the user just stopped.)
    m.redraw();
  }

  onremove() {
    this.stopPlayback();
    if (this.audioCtx) {
      void this.audioCtx.close();
      this.audioCtx = null;
    }
  }
}
