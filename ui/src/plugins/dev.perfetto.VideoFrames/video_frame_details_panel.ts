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
import {NUM, STR} from '../../trace_processor/query_result';
import {Timestamp} from '../../components/widgets/timestamp';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {TrackEventSelection} from '../../public/selection';
import {Button, ButtonBar} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Select} from '../../widgets/select';
import {Tree, TreeNode} from '../../widgets/tree';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import type {SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
import type {Row} from '../../trace_processor/query_result';
import type {VideoFramePlayer} from './video_frame_player';

// Playback speed options, in real-time multiples.
const PLAYBACK_RATES = [0.1, 0.2, 0.5, 1, 1.5, 2];

interface BufferRow {
  readonly layer: string;
  readonly visible: string; // 'yes' | 'no'
  readonly bufferFrame: number;
  readonly state: string; // 'updated' | 'reused'
  readonly size: string;
  readonly z: number;
}

const BUFFER_SCHEMA: SchemaRegistry = {
  root: {
    layer: {title: 'Layer'},
    visible: {title: 'Visible'},
    bufferFrame: {title: 'Buffer'},
    state: {title: 'State'},
    size: {title: 'Size'},
    z: {title: 'Z'},
  },
};

export class VideoFrameDetailsPanel implements TrackEventDetailsPanel {
  private readonly player: VideoFramePlayer;

  constructor(player: VideoFramePlayer) {
    this.player = player;
  }

  private buffers?: ReadonlyArray<BufferRow>;
  private buffersDataSource?: InMemoryDataSource;
  private buffersTs?: bigint;

  // Layers holding a buffer in the SurfaceFlinger snapshot nearest the frame,
  // flagged reused when the buffer is unchanged from the prior snapshot.
  private async loadBuffers(
    frameTs: bigint,
  ): Promise<ReadonlyArray<BufferRow>> {
    try {
      const result = await this.player.trace.engine.query(`
        with snap as (
          select id, ts from surfaceflinger_layers_snapshot
          where ts <= ${frameTs} order by ts desc limit 1
        ),
        prev as (
          select id from surfaceflinger_layers_snapshot
          where ts < (select ts from snap) order by ts desc limit 1
        )
        select
          l.layer_name as layer,
          case when l.is_visible then 'yes' else 'no' end as visible,
          extract_arg(l.arg_set_id, 'curr_frame') as bufferFrame,
          case when extract_arg(l.arg_set_id, 'curr_frame') = (
            select extract_arg(pl.arg_set_id, 'curr_frame')
            from surfaceflinger_layer pl
            where pl.snapshot_id = (select id from prev)
              and pl.layer_name = l.layer_name)
          then 'reused' else 'updated' end as state,
          extract_arg(l.arg_set_id, 'active_buffer.width') || 'x' ||
            extract_arg(l.arg_set_id, 'active_buffer.height') as size,
          coalesce(extract_arg(l.arg_set_id, 'z'), 0) as z
        from surfaceflinger_layer l
        where l.snapshot_id = (select id from snap)
          and extract_arg(l.arg_set_id, 'active_buffer.width') is not null
        order by l.is_visible desc, z
      `);
      const rows: BufferRow[] = [];
      const it = result.iter({
        layer: STR,
        visible: STR,
        bufferFrame: NUM,
        state: STR,
        size: STR,
        z: NUM,
      });
      for (; it.valid(); it.next()) {
        rows.push({
          layer: it.layer,
          visible: it.visible,
          bufferFrame: it.bufferFrame,
          state: it.state,
          size: it.size,
          z: it.z,
        });
      }
      return rows;
    } catch {
      // Trace has no android.surfaceflinger.layers data source.
      return [];
    }
  }

  private renderBuffers(frameTs: bigint): m.Children {
    if (this.buffersTs !== frameTs) {
      this.buffersTs = frameTs;
      this.buffers = undefined;
      this.buffersDataSource = undefined;
      void this.loadBuffers(frameTs).then((rows) => {
        if (this.buffersTs === frameTs) {
          this.buffers = rows;
          m.redraw();
        }
      });
    }
    if (this.buffers === undefined) return m('span', 'Loading…');
    if (this.buffers.length === 0) {
      return m(
        'span',
        'No buffers for this frame (the trace may lack the ' +
          'android.surfaceflinger.layers data source).',
      );
    }
    // Cached so the grid keeps its sort/filter state; reset on frame change.
    this.buffersDataSource ??= new InMemoryDataSource(
      this.buffers as unknown as ReadonlyArray<Row>,
    );
    return m(DataGrid, {
      schema: BUFFER_SCHEMA,
      rootSchema: 'root',
      data: this.buffersDataSource,
      fillHeight: false,
    });
  }

  async load(sel: TrackEventSelection) {
    // Skip re-decode when the selection update was triggered by us
    // (playback loop, next/prev, or the player's own selectAndSeek path).
    // The player already has currentIdx pointing at this event.
    if (this.player.playing) return;
    await this.player.ensureFramesLoaded();
    if (this.player.frames[this.player.currentIdx]?.id === sel.eventId) return;
    await this.player.seek(sel.eventId);
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
        m(
          GridLayoutColumn,
          m(Section, {title: 'Details'}, m(Tree, detailRows)),
          m(Section, {title: 'Buffers'}, this.renderBuffers(frame.ts)),
        ),
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

  private async jumpToSurfaceFlingerFrame() {
    const frame = this.player.currentFrame;
    if (frame === undefined) return;
    const trace = this.player.trace;
    // A video frame shows the last presented composition: the latest presented
    // display frame (surface-token-null) at/before it, skipping dropped frames
    // (named "0"), which were never on screen.
    const result = await trace.engine.query(`
      select id, track_id as trackId
      from actual_frame_timeline_slice
      where extract_arg(arg_set_id, 'Surface frame token') is null
        and extract_arg(arg_set_id, 'Present type') != 'Dropped Frame'
        and name != '0'
        and ts + dur <= ${frame.ts}
      order by ts + dur desc
      limit 1
    `);
    if (result.numRows() === 0) return;
    const {id, trackId} = result.firstRow({id: NUM, trackId: NUM});
    // These slices live on the Frames plugin's "Actual Timeline" track, keyed
    // by actual_frame_timeline_slice.id; find that track and select there.
    const track = trace.tracks
      .getAllTracks()
      .find((t) => t.tags?.trackIds?.includes(trackId));
    if (track === undefined) return;
    trace.selection.selectTrackEvent(track.uri, id, {scrollToSelection: true});
  }

  private renderControls(): m.Children {
    const p = this.player;
    const idx = p.currentIdx;
    const total = p.frames.length;
    return m(
      ButtonBar,
      m(Button, {
        label: 'Jump to SF frame',
        icon: 'arrow_forward',
        compact: true,
        onclick: () => this.jumpToSurfaceFlingerFrame(),
      }),
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
