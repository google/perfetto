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
import {materialColorScheme} from '../../components/colorizer';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {TrackEventSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import {Button} from '../../widgets/button';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';

interface BufferSlice {
  readonly vsync: number | null;
  readonly process: string;
  readonly layer: string;
  readonly buffer: number;
  readonly state: string;
  readonly visible: string;
  readonly size: string;
  readonly format: number;
  readonly z: number;
  readonly hwc: string;
  // The Choreographer#doFrame that produced this buffer, or null when there is
  // none (a held buffer, or a non-app layer such as wallpaper).
  readonly doFrameId: number | null;
}

// Details panel for a buffer slice: the full layer/buffer info at that frame.
class BufferSliceDetailsPanel implements TrackEventDetailsPanel {
  private slice?: BufferSlice;

  constructor(private readonly trace: Trace) {}

  async load(sel: TrackEventSelection) {
    const result = await this.trace.engine.query(`
      select
        b.process,
        extract_arg(sf.arg_set_id, 'Surface frame token') as vsync,
        -- The doFrame that drew it: the Choreographer#doFrame with the surface
        -- frame's vsync token, in the frame's process (the token repeats across
        -- every process that rendered that vsync).
        (select ds.id from slice ds
         join thread_track dtt on dtt.id = ds.track_id
         join thread dth on dth.utid = dtt.utid
         where dth.upid = extract_arg(sft.dimension_arg_set_id, 'upid')
           and ds.name = 'Choreographer#doFrame ' ||
                         extract_arg(sf.arg_set_id, 'Surface frame token'))
          as doFrameId,
        b.layer_name as layer,
        b.buffer_frame as buffer,
        case when b.changed then 'updated' else 'reused' end as state,
        case when b.is_visible then 'yes' else 'no' end as visible,
        b.buffer_width || 'x' || b.buffer_height as size,
        b.buffer_format as format,
        b.z_order as z,
        case b.hwc_composition_type
          when 1 then 'Client (GPU)'
          when 2 then 'Device (HWC)'
          when 3 then 'Solid color'
          when 4 then 'Cursor'
          when 5 then 'Sideband'
          when 6 then 'Display decoration'
          else cast(b.hwc_composition_type as text) end as hwc
      from _video_frame_buffer_slices b
      -- This layer's buffer surface frame in its composite (Is Buffer? -- a
      -- token can also carry animation frames), matched by layer name.
      left join actual_frame_timeline_slice sf
        on extract_arg(sf.arg_set_id, 'Is Buffer?') = 'Yes'
       and cast(extract_arg(sf.arg_set_id, 'Display frame token') as int) =
           b.composite_token
       and _vf_strip_vri(
             replace(extract_arg(sf.arg_set_id, 'Layer name'), 'TX - ', ''))
           glob b.track_name || '#*'
      left join track sft on sft.id = sf.track_id
      where b.id = ${sel.eventId}
    `);
    if (result.numRows() === 0) {
      this.slice = undefined;
      return;
    }
    this.slice = result.firstRow({
      vsync: NUM_NULL,
      process: STR,
      layer: STR,
      buffer: NUM,
      state: STR,
      visible: STR,
      size: STR,
      format: NUM,
      z: NUM,
      hwc: STR,
      doFrameId: NUM_NULL,
    });
  }

  private jumpToDoFrame(id: number) {
    this.trace.selection.selectSqlEvent('slice', id, {scrollToSelection: true});
  }

  render() {
    const s = this.slice;
    if (s === undefined) {
      return m(DetailsShell, {title: 'Buffer'}, m('span', 'Loading…'));
    }
    return m(
      DetailsShell,
      {
        title: 'Buffer',
        description: s.process,
        buttons:
          s.doFrameId !== null &&
          m(Button, {
            label: 'Jump to doFrame',
            icon: 'arrow_forward',
            onclick: () => this.jumpToDoFrame(s.doFrameId!),
          }),
      },
      m(
        GridLayout,
        m(
          Section,
          {title: 'Details'},
          m(Tree, [
            m(TreeNode, {left: 'Process', right: s.process}),
            s.vsync !== null &&
              m(TreeNode, {left: 'Vsync', right: `${s.vsync}`}),
            m(TreeNode, {left: 'Layer', right: s.layer}),
            m(TreeNode, {left: 'Buffer', right: `${s.buffer}`}),
            m(TreeNode, {left: 'State', right: s.state}),
            m(TreeNode, {left: 'Visible', right: s.visible}),
            m(TreeNode, {left: 'Size', right: s.size}),
            m(TreeNode, {left: 'Format', right: `${s.format}`}),
            m(TreeNode, {left: 'HWC composition', right: s.hwc}),
            m(TreeNode, {left: 'Z-order', right: `${s.z}`}),
          ]),
        ),
      ),
    );
  }
}

// A track of one layer's buffer as slices aligned to the video frames it was on
// screen for. With changedOnly, only frames with a fresh buffer become slices.
export function createLayerTrack(
  trace: Trace,
  uri: string,
  trackIdx: number,
  changedOnly: boolean,
) {
  const src = `
    SELECT id, ts, dur, 0 AS depth, name, track_name
    FROM _video_frame_buffer_slices
    WHERE track_idx = ${trackIdx}${changedOnly ? ' AND changed = 1' : ''}
  `;
  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        name: STR,
        depth: NUM,
        track_name: STR,
      },
      src,
    }),
    // Label by vsync (name), but colour by layer so one track stays one colour.
    colorizer: (row) => materialColorScheme(row.track_name),
    detailsPanel: () => new BufferSliceDetailsPanel(trace),
  });
}
