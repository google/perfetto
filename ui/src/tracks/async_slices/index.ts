// Copyright (C) 2021 The Android Open Source Project
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

import {BigintMath as BIMath} from '../../base/bigint_math';
import {duration, time} from '../../base/time';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../common/query_result';
import {SliceData, SliceTrackBase} from '../../frontend/slice_track_base';
import {
  EngineProxy,
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {getTrackName} from '../../public/utils';

export const ASYNC_SLICE_TRACK_KIND = 'AsyncSliceTrack';

class AsyncSliceTrack extends SliceTrackBase {
  private maxDurNs: duration = 0n;

  constructor(
      private engine: EngineProxy, maxDepth: number, trackKey: string,
      private trackIds: number[], namespace?: string) {
    // TODO is 'slice' right here?
    super(maxDepth, trackKey, 'slice', namespace);
  }

  async onBoundsChange(start: time, end: time, resolution: duration):
      Promise<SliceData> {
    if (this.maxDurNs === 0n) {
      const maxDurResult = await this.engine.query(`
        select max(iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts,
        dur)) as maxDur from experimental_slice_layout where filter_track_ids
        = '${this.trackIds.join(',')}'
      `);
      this.maxDurNs = maxDurResult.firstRow({maxDur: LONG_NULL}).maxDur || 0n;
    }

    const queryRes = await this.engine.query(`
      SELECT
      (ts + ${resolution / 2n}) / ${resolution} * ${resolution} as tsq,
        ts,
        max(iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts, dur)) as
        dur, layout_depth as depth, ifnull(name, '[null]') as name, id, dur =
        0 as isInstant, dur = -1 as isIncomplete
      from experimental_slice_layout
      where
        filter_track_ids = '${this.trackIds.join(',')}' and
        ts >= ${start - this.maxDurNs} and
        ts <= ${end}
      group by tsq, layout_depth
      order by tsq, layout_depth
    `);

    const numRows = queryRes.numRows();
    const slices: SliceData = {
      start,
      end,
      resolution,
      length: numRows,
      strings: [],
      sliceIds: new Float64Array(numRows),
      starts: new BigInt64Array(numRows),
      ends: new BigInt64Array(numRows),
      depths: new Uint16Array(numRows),
      titles: new Uint16Array(numRows),
      isInstant: new Uint16Array(numRows),
      isIncomplete: new Uint16Array(numRows),
    };

    const stringIndexes = new Map<string, number>();
    function internString(str: string) {
      let idx = stringIndexes.get(str);
      if (idx !== undefined) return idx;
      idx = slices.strings.length;
      slices.strings.push(str);
      stringIndexes.set(str, idx);
      return idx;
    }

    const it = queryRes.iter({
      tsq: LONG,
      ts: LONG,
      dur: LONG,
      depth: NUM,
      name: STR,
      id: NUM,
      isInstant: NUM,
      isIncomplete: NUM,
    });
    for (let row = 0; it.valid(); it.next(), row++) {
      const startQ = it.tsq;
      const start = it.ts;
      const dur = it.dur;
      const end = start + dur;
      const minEnd = startQ + resolution;
      const endQ = BIMath.max(BIMath.quant(end, resolution), minEnd);

      slices.starts[row] = startQ;
      slices.ends[row] = endQ;
      slices.depths[row] = it.depth;
      slices.titles[row] = internString(it.name);
      slices.sliceIds[row] = it.id;
      slices.isInstant[row] = it.isInstant;
      slices.isIncomplete[row] = it.isIncomplete;
    }
    return slices;
  }
}

class AsyncSlicePlugin implements Plugin {
  onActivate(_ctx: PluginContext) {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    await this.addGlobalAsyncTracks(ctx);
    await this.addProcessAsyncSliceTracks(ctx);
  }

  async addGlobalAsyncTracks(ctx: PluginContextTrace): Promise<void> {
    const {engine} = ctx;
    const rawGlobalAsyncTracks = await engine.query(`
      with tracks_with_slices as materialized (
        select distinct track_id
        from slice
      ),
      global_tracks as (
        select
          track.parent_id as parent_id,
          track.id as track_id,
          track.name as name
        from track
        join tracks_with_slices on tracks_with_slices.track_id = track.id
        where
          track.type = "track"
          or track.type = "gpu_track"
          or track.type = "cpu_track"
      ),
      global_tracks_grouped as (
        select
          parent_id,
          name,
          group_concat(track_id) as trackIds,
          count(track_id) as trackCount
        from global_tracks track
        group by parent_id, name
      )
      select
        t.parent_id as parentId,
        p.name as parentName,
        t.name as name,
        t.trackIds as trackIds,
        max_layout_depth(t.trackCount, t.trackIds) as maxDepth
      from global_tracks_grouped AS t
      left join track p on (t.parent_id = p.id)
      order by p.name, t.name;
    `);
    const it = rawGlobalAsyncTracks.iter({
      name: STR_NULL,
      parentName: STR_NULL,
      parentId: NUM_NULL,
      trackIds: STR,
      maxDepth: NUM_NULL,
    });

    // let scrollJankRendered = false;

    for (; it.valid(); it.next()) {
      const rawName = it.name === null ? undefined : it.name;
      // const rawParentName = it.parentName === null ? undefined :
      // it.parentName;
      const displayName = getTrackName({name: rawName, kind: 'AsyncSlice'});
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      // const parentTrackId = it.parentId;
      const maxDepth = it.maxDepth;

      // If there are no slices in this track, skip it.
      if (maxDepth === null) {
        continue;
      }

      // if (ENABLE_SCROLL_JANK_PLUGIN_V2.get() && !scrollJankRendered &&
      //     name.includes(INPUT_LATENCY_TRACK)) {
      //   // This ensures that the scroll jank tracks render above the tracks
      //   // for GestureScrollUpdate.
      //   await this.addScrollJankTracks(this.engine);
      //   scrollJankRendered = true;
      // }

      ctx.registerStaticTrack({
        uri: `perfetto.AsyncSlices#${rawName}`,
        displayName,
        trackIds,
        kind: ASYNC_SLICE_TRACK_KIND,
        track: ({trackKey}) => {
          return new AsyncSliceTrack(
              engine,
              maxDepth,
              trackKey,
              trackIds,
          );
        },
      });
    }
  }

  async addProcessAsyncSliceTracks(ctx: PluginContextTrace): Promise<void> {
    const result = await ctx.engine.query(`
      with process_async_tracks as materialized (
        select
          process_track.upid as upid,
          process_track.name as trackName,
          process.name as processName,
          process.pid as pid,
          group_concat(process_track.id) as trackIds,
          count(1) as trackCount
        from process_track
        left join process using(upid)
        where
            process_track.name is null or
            process_track.name not like "% Timeline"
        group by
          process_track.upid,
          process_track.name
      )
      select
        t.*,
        max_layout_depth(t.trackCount, t.trackIds) as maxDepth
      from process_async_tracks t;
    `);

    const it = result.iter({
      upid: NUM,
      trackName: STR_NULL,
      trackIds: STR,
      processName: STR_NULL,
      pid: NUM_NULL,
      maxDepth: NUM_NULL,
    });
    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const trackName = it.trackName;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const processName = it.processName;
      const pid = it.pid;
      const maxDepth = it.maxDepth;

      if (maxDepth === null) {
        // If there are no slices in this track, skip it.
        continue;
      }

      const kind = ASYNC_SLICE_TRACK_KIND;
      const displayName =
          getTrackName({name: trackName, upid, pid, processName, kind});

      ctx.registerStaticTrack({
        uri: `perfetto.AsyncSlices#process.${pid}${rawTrackIds}`,
        displayName,
        trackIds,
        kind: ASYNC_SLICE_TRACK_KIND,
        track: ({trackKey}) => {
          return new AsyncSliceTrack(
              ctx.engine,
              maxDepth,
              trackKey,
              trackIds,
          );
        },
      });
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.AsyncSlices',
  plugin: AsyncSlicePlugin,
};
