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
import {LONG_NULL, NUM, STR_NULL} from '../../trace_processor/query_result';
import {Anchor} from '../../widgets/anchor';
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

// One app (SurfaceFrame) that was composited into this frame's DisplayFrame.
interface AppFrame {
  sliceId: number; // actual_frame_timeline_slice.id, to jump to
  token?: bigint; // surface_frame_token: this app's own vsync id
  process: string; // process name (falls back to layer name)
}

export class VideoFrameDetailsPanel implements TrackEventDetailsPanel {
  private readonly player: VideoFramePlayer;

  // App frames composited into the current frame's DisplayFrame, cached per
  // vsync id so we query at most once as long as the selection stays put.
  private appFramesVsyncId?: bigint;
  private appFramesLoaded = false;
  private appFrames: AppFrame[] = [];

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
    if (frame.vsyncId !== undefined) {
      const vsyncId = frame.vsyncId;
      // Fetch (once per vsync id) the app frames composited into this
      // DisplayFrame, shown nested below so you can jump straight to an app's
      // frame timeline without the SurfaceFlinger round trip.
      this.ensureAppFrames(vsyncId);
      detailRows.push(
        m(
          TreeNode,
          {
            left: 'Frame timeline vsync id',
            // Clickable (highlighted like the Timestamp): jump to the
            // SurfaceFlinger DisplayFrame (frame-timeline slice) this frame
            // was composited in.
            right: m(
              Anchor,
              {onclick: () => this.jumpToDisplayFrame(vsyncId)},
              `${vsyncId}`,
            ),
          },
          this.renderAppFrameNodes(vsyncId),
        ),
      );
    }
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

  // Selects and scrolls to the SurfaceFlinger DisplayFrame (frame-timeline
  // slice) with this composite token, so you can jump from a captured video
  // frame to the composite that produced it.
  private async jumpToDisplayFrame(vsyncId: bigint) {
    const trace = this.player.trace;
    const res = await trace.engine.query(`
      SELECT id
      FROM actual_frame_timeline_slice
      WHERE display_frame_token = ${vsyncId} AND layer_name IS NULL
      ORDER BY ts
      LIMIT 1
    `);
    if (res.numRows() === 0) return;
    const id = res.firstRow({id: NUM}).id;
    trace.selection.selectSqlEvent('slice', id, {scrollToSelection: true});
  }

  // Loads the app frames (SurfaceFrames) that were composited into the
  // DisplayFrame with this token, unless already cached for this vsync id.
  // A SurfaceFrame carries the display_frame_token of the DisplayFrame it was
  // presented in, so this needs no round trip through SurfaceFlinger.
  private ensureAppFrames(vsyncId: bigint) {
    if (this.appFramesVsyncId === vsyncId) return; // already loaded or loading
    this.appFramesVsyncId = vsyncId;
    this.appFramesLoaded = false;
    this.appFrames = [];
    void this.loadAppFrames(vsyncId);
  }

  private async loadAppFrames(vsyncId: bigint) {
    const res = await this.player.trace.engine.query(`
      SELECT
        s.id AS sliceId,
        s.surface_frame_token AS token,
        COALESCE(p.name, s.layer_name) AS process
      FROM actual_frame_timeline_slice s
      LEFT JOIN process p USING (upid)
      WHERE s.display_frame_token = ${vsyncId} AND s.layer_name IS NOT NULL
      ORDER BY process, token
    `);
    // A newer selection superseded us while the query was in flight.
    if (this.appFramesVsyncId !== vsyncId) return;
    const rows: AppFrame[] = [];
    const it = res.iter({sliceId: NUM, token: LONG_NULL, process: STR_NULL});
    for (; it.valid(); it.next()) {
      rows.push({
        sliceId: it.sliceId,
        token: it.token ?? undefined,
        process: it.process ?? '<unknown>',
      });
    }
    this.appFrames = rows;
    this.appFramesLoaded = true;
    m.redraw();
  }

  // The app-frame rows shown nested under the SF vsync id: process name on the
  // left, the app's own vsync id (surface_frame_token) as a clickable link on
  // the right that jumps straight to that app's frame timeline slice.
  private renderAppFrameNodes(vsyncId: bigint): m.Children {
    if (!this.appFramesLoaded || this.appFramesVsyncId !== vsyncId) {
      return m(TreeNode, {left: 'Loading app frames…'});
    }
    if (this.appFrames.length === 0) {
      return m(TreeNode, {left: 'No app frames'});
    }
    return this.appFrames.map((af) =>
      m(TreeNode, {
        left: af.process,
        right:
          af.token === undefined
            ? '(no vsync id)'
            : m(
                Anchor,
                {
                  onclick: () =>
                    this.player.trace.selection.selectSqlEvent(
                      'slice',
                      af.sliceId,
                      {scrollToSelection: true},
                    ),
                },
                `${af.token}`,
              ),
      }),
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
    );
  }
}
