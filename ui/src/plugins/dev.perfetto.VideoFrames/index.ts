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

import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {NUM, STR, STR_NULL} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {VideoFramePlayer} from './video_frame_player';
import {createVideoFramesTrack} from './video_frames_track';
import {createLayerTrack} from './layer_track';

interface StreamInfo {
  displayId: number;
  displayName: string;
}

// One row per (video frame, visible buffered layer): the buffer on screen for
// that frame. track_idx groups rows into one track per layer; changed flags a
// fresh buffer vs a held one.
const LAYER_BUFFER_SLICES = `
  with video_frame as (
    select ts as video_ts,
      lead(ts) over (order by ts) - ts as video_dur
    from __intrinsic_video_frames where coalesce(is_config, 0) = 0
  ),
  -- SF composites (one per on-screen frame); the name holds the display frame
  -- token. Resolved once here so the details panel is a cheap per-slice lookup.
  composite as materialized (
    select cast(str_split(c.name, ' ', 1) as int) as token,
      c.ts + c.dur as present
    from slice c
    join thread_track tt on tt.id = c.track_id
    join thread t on t.utid = tt.utid
    join process p on p.upid = t.upid
    where p.name glob '*surfaceflinger*' and c.name glob 'composite *'
  ),
  frame_snapshot as (
    select video_ts, video_dur,
      (select max(s.ts) from surfaceflinger_layers_snapshot s
       where s.ts <= video_frame.video_ts) as snapshot_ts,
      -- the composite on screen at this video frame (its display frame token)
      (select c.token from composite c
       where c.present <= video_frame.video_ts
       order by c.present desc limit 1) as composite_token
    from video_frame where video_dur is not null
  ),
  -- Each rendered layer's process, from its app frames. A layer is owned by one
  -- process, so this is stable -- it gives the process even for a held frame.
  track_process as (
    select
      str_split(_vf_strip_vri(
        replace(extract_arg(sf.arg_set_id, 'Layer name'), 'TX - ', '')),
        '#', 0) as track_name,
      max((select p.name from process p
           where p.upid = extract_arg(t.dimension_arg_set_id, 'upid'))) as process
    from actual_frame_timeline_slice sf
    join track t on t.id = sf.track_id
    where extract_arg(sf.arg_set_id, 'Is Buffer?') = 'Yes'
    group by 1
  ),
  raw_layer as (
    select s.ts as snapshot_ts, l.layer_name, l.arg_set_id,
      l.hwc_composition_type, l.is_visible,
      -- track_name: the layer name without the ViewRootImpl wrapper or #id.
      str_split(_vf_strip_vri(l.layer_name), '#', 0) as track_name
    from surfaceflinger_layers_snapshot s
    join surfaceflinger_layer l on l.snapshot_id = s.id
    where l.is_visible = 1
      and extract_arg(l.arg_set_id, 'active_buffer.width') is not null
  ),
  layer as (
    select raw_layer.*,
      -- The layer's process: its renderer, or -- for a layer that never renders
      -- (snapshot, wallpaper) -- its surface owner (uid).
      coalesce(
        tp.process,
        (select p.name from process p
         where p.uid = extract_arg(raw_layer.arg_set_id, 'owner_uid') limit 1),
        raw_layer.track_name) as process
    from raw_layer
    left join track_process tp on tp.track_name = raw_layer.track_name
  ),
  -- The app frame's vsync token per (layer, composite), matched on display frame
  -- token + layer name. Used to label the slice.
  surface_frame as materialized (
    select
      cast(extract_arg(sf.arg_set_id, 'Display frame token') as int) as token,
      str_split(_vf_strip_vri(
        replace(extract_arg(sf.arg_set_id, 'Layer name'), 'TX - ', '')),
        '#', 0) as track_name,
      cast(extract_arg(sf.arg_set_id, 'Surface frame token') as int) as vsync
    from actual_frame_timeline_slice sf
    where extract_arg(sf.arg_set_id, 'Is Buffer?') = 'Yes'
  ),
  buffer_slice as (
    select f.video_ts as ts, f.video_dur as dur, f.composite_token,
      layer.track_name, layer.process, layer.layer_name,
      -- slice label: the app frame's vsync token, or 'NULL' if the layer did
      -- not (re)draw this frame (a held buffer).
      coalesce(cast(sfr.vsync as text), 'NULL') as name,
      extract_arg(layer.arg_set_id, 'curr_frame') as buffer_frame,
      extract_arg(layer.arg_set_id, 'active_buffer.width') as buffer_width,
      extract_arg(layer.arg_set_id, 'active_buffer.height') as buffer_height,
      extract_arg(layer.arg_set_id, 'active_buffer.format') as buffer_format,
      coalesce(extract_arg(layer.arg_set_id, 'z'), 0) as z_order,
      layer.hwc_composition_type, layer.is_visible
    from frame_snapshot f
    join layer on layer.snapshot_ts = f.snapshot_ts
    left join surface_frame sfr
      on sfr.token = f.composite_token and sfr.track_name = layer.track_name
  )
  select row_number() over (order by track_name, ts) as id,
    ts, dur, name, track_name, process, composite_token,
    layer_name, buffer_frame, buffer_width,
    buffer_height, buffer_format, z_order, hwc_composition_type, is_visible,
    dense_rank() over (order by track_name) as track_idx,
    case when buffer_frame is distinct from
         lag(buffer_frame) over (partition by track_name order by ts)
         then 1 else 0 end as changed
  from buffer_slice
`;

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.VideoFrames';
  static readonly description =
    'Shows display frames captured by the android.display.video data source. ' +
    'Adds a per-display timeline track with a decoded frame preview and ' +
    'playback.';

  async onTraceLoad(ctx: Trace): Promise<void> {
    const res = await ctx.engine.query(`
      SELECT display_id AS displayId, MAX(display_name) AS displayName
      FROM __intrinsic_video_frames
      GROUP BY display_id
      ORDER BY display_id
    `);

    const streams: StreamInfo[] = [];
    const it = res.iter({displayId: NUM, displayName: STR_NULL});
    for (; it.valid(); it.next()) {
      streams.push({
        displayId: it.displayId,
        displayName: it.displayName ?? `Display ${it.displayId}`,
      });
    }
    if (streams.length === 0) return;

    const group = new TrackNode({
      name: 'Video Frames',
      isSummary: true,
      sortOrder: -55,
    });

    for (const stream of streams) {
      const uri = `/video_frames/${stream.displayId}`;
      const player = new VideoFramePlayer(ctx, uri, stream.displayId);

      ctx.tracks.registerTrack({
        uri,
        renderer: createVideoFramesTrack(ctx, uri, stream.displayId, player),
      });
      group.addChildInOrder(new TrackNode({uri, name: stream.displayName}));
    }

    ctx.defaultWorkspace.addChildInOrder(group);

    await this.addLayerTracks(ctx);
  }

  // A "Layers" group, one track per layer, showing the buffer as slices aligned
  // to the video frames (split into All / Changed). Absent without layers data.
  private async addLayerTracks(ctx: Trace): Promise<void> {
    const count = await ctx.engine.query(
      `select count(*) as c from surfaceflinger_layers_snapshot`,
    );
    if (count.firstRow({c: NUM}).c === 0) return;

    // Strip the ViewRootImpl wrapper ("VRI[<name>]" / "VRI-<name>") off a layer
    // name, so the track name and the frame-timeline layer match are the app's.
    await ctx.engine.query(`
      create perfetto function _vf_strip_vri(name string) returns string as
      select case
        when $name glob 'VRI[[]*' then substr($name, 5, instr($name, ']') - 5)
        when $name glob 'VRI-*' then substr($name, 5)
        else $name
      end
    `);
    await createPerfettoTable({
      engine: ctx.engine,
      name: '_video_frame_buffer_slices',
      as: LAYER_BUFFER_SLICES,
    });

    const res = await ctx.engine.query(`
      select track_idx as trackIdx, track_name as trackName
      from _video_frame_buffer_slices group by 1, 2 order by 1
    `);

    const group = new TrackNode({
      name: 'Layers',
      isSummary: true,
      sortOrder: -54,
    });
    const allGroup = new TrackNode({name: 'All', isSummary: true});
    const changedGroup = new TrackNode({name: 'Changed', isSummary: true});

    const it = res.iter({trackIdx: NUM, trackName: STR});
    for (; it.valid(); it.next()) {
      const {trackIdx, trackName} = it;

      const allUri = `/video_frame_layers/all/${trackIdx}`;
      ctx.tracks.registerTrack({
        uri: allUri,
        renderer: createLayerTrack(ctx, allUri, trackIdx, false),
      });
      allGroup.addChildInOrder(new TrackNode({uri: allUri, name: trackName}));

      const changedUri = `/video_frame_layers/changed/${trackIdx}`;
      ctx.tracks.registerTrack({
        uri: changedUri,
        renderer: createLayerTrack(ctx, changedUri, trackIdx, true),
      });
      changedGroup.addChildInOrder(
        new TrackNode({uri: changedUri, name: trackName}),
      );
    }

    group.addChildInOrder(allGroup);
    group.addChildInOrder(changedGroup);
    ctx.defaultWorkspace.addChildInOrder(group);
  }
}
