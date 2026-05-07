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
import {Trace} from '../../public/trace';
import {
  AreaSelection,
  AreaSelectionTab,
  ContentWithLoadingFlag,
} from '../../public/selection';
import {BLOB, LONG, NUM} from '../../trace_processor/query_result';
import {Button, ButtonBar} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {Select} from '../../widgets/select';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {Timestamp} from '../../components/widgets/timestamp';
import {Time} from '../../base/time';
import {
  FrameInfo,
  FPS_OPTIONS,
  getSessionFps,
  setSessionFps,
} from './playback_state';

export class VideoFramesSelectionTab implements AreaSelectionTab {
  readonly id: string;
  readonly name: string;
  readonly priority = 10;

  private readonly trace: Trace;
  private readonly trackUri: string;
  private readonly trackId: number;
  private frames: FrameInfo[] = [];
  private currentIdx = 0;
  private imageUrl?: string;
  private playing = false;
  private playTimer?: ReturnType<typeof setInterval>;
  private loading = false;
  private lastSelectionKey = '';
  private playbackStartIdx = 0;

  constructor(
    trace: Trace,
    trackUri: string,
    trackId: number,
    trackName: string,
  ) {
    this.trace = trace;
    this.trackUri = trackUri;
    this.trackId = trackId;
    this.id = `video_frames_playback_${trackId}`;
    this.name = trackName;
  }

  render(selection: AreaSelection): ContentWithLoadingFlag | undefined {
    if (!selection.trackUris.includes(this.trackUri)) return undefined;

    const key = `${selection.start}-${selection.end}`;
    if (key !== this.lastSelectionKey) {
      this.lastSelectionKey = key;
      this.stop();
      this.loadFramesForSelection(selection);
    }

    if (this.frames.length === 0 && !this.loading) {
      return undefined;
    }

    const idx = this.currentIdx;
    const total = this.frames.length;
    const frame = this.frames[idx];

    return {
      isLoading: this.loading,
      buttons: this.renderControls(idx, total),
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
                  right: `${idx + 1} of ${total}`,
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
          this.imageUrl
            ? m('img.pf-video-frame-preview', {src: this.imageUrl})
            : m('span', this.loading ? '' : 'No image'),
        ),
      ),
    };
  }

  private renderControls(idx: number, total: number): m.Children {
    return m(ButtonBar, [
      m(Button, {
        icon: 'skip_previous',
        compact: true,
        disabled: idx <= 0 || this.playing,
        onclick: () => this.prev(),
      }),
      m(Button, {
        icon: this.playing ? 'pause' : 'play_arrow',
        intent: this.playing ? Intent.Warning : Intent.Primary,
        compact: true,
        onclick: () => this.togglePlay(),
      }),
      m(Button, {
        icon: 'skip_next',
        compact: true,
        disabled: idx >= total - 1 || this.playing,
        onclick: () => this.next(),
      }),
      m(
        Select,
        {
          value: String(getSessionFps()),
          onchange: (e: Event) => {
            setSessionFps(Number((e.target as HTMLSelectElement).value));
            if (this.playing) {
              this.play();
            }
          },
        },
        FPS_OPTIONS.map((f) =>
          m(
            'option',
            {value: String(f), selected: f === getSessionFps()},
            `${f} fps`,
          ),
        ),
      ),
    ]);
  }

  private async loadFramesForSelection(selection: AreaSelection) {
    this.loading = true;
    this.frames = [];
    this.currentIdx = 0;
    m.redraw();

    const res = await this.trace.engine.query(`
      SELECT id, ts, frame_number AS frameNumber
      FROM android_video_frames
      WHERE COALESCE(track_id, 0) = ${this.trackId}
        AND ts >= ${selection.start} AND ts <= ${selection.end}
      ORDER BY ts
    `);
    const it = res.iter({id: NUM, ts: LONG, frameNumber: NUM});
    for (; it.valid(); it.next()) {
      this.frames.push({id: it.id, ts: it.ts, frameNumber: it.frameNumber});
    }
    this.loading = false;

    if (this.frames.length > 0) {
      await this.loadImage(0);
    }
    m.redraw();
  }

  private async loadImage(idx: number) {
    if (idx < 0 || idx >= this.frames.length) return;
    this.currentIdx = idx;

    if (this.imageUrl) {
      URL.revokeObjectURL(this.imageUrl);
      this.imageUrl = undefined;
    }

    const id = this.frames[idx].id;
    const res = await this.trace.engine.query(
      `SELECT video_frame_image(${id}) AS img`,
    );
    const row = res.firstRow({img: BLOB});
    if (row.img.length > 0) {
      const blob = new Blob([row.img]);
      this.imageUrl = URL.createObjectURL(blob);
    }
    m.redraw();
  }

  private prev() {
    if (this.currentIdx > 0) this.loadImage(this.currentIdx - 1);
  }

  private next() {
    if (this.currentIdx < this.frames.length - 1) {
      this.loadImage(this.currentIdx + 1);
    }
  }

  private togglePlay() {
    if (this.playing) {
      this.stop();
    } else {
      this.play();
    }
    m.redraw();
  }

  private play() {
    this.stop();
    this.playing = true;
    this.playbackStartIdx = this.currentIdx;
    this.playTimer = setInterval(() => {
      if (this.currentIdx < this.frames.length - 1) {
        this.loadImage(this.currentIdx + 1);
      } else {
        this.stop();
        this.loadImage(this.playbackStartIdx);
      }
    }, 1000 / getSessionFps());
  }

  private stop() {
    this.playing = false;
    if (this.playTimer !== undefined) {
      clearInterval(this.playTimer);
      this.playTimer = undefined;
    }
  }
}
