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

export const ACTUAL_FRAMES_SLICE_TRACK_KIND = 'ActualFramesSliceTrack';

const BLUE_COLOR = '#03A9F4';         // Blue 500
const GREEN_COLOR = '#4CAF50';        // Green 500
const YELLOW_COLOR = '#FFEB3B';       // Yellow 500
const RED_COLOR = '#FF5722';          // Red 500
const LIGHT_GREEN_COLOR = '#C0D588';  // Light Green 500
const PINK_COLOR = '#F515E0';         // Pink 500

class SliceTrack extends SliceTrackBase {
  private maxDur = 0n;

  constructor(
      private engine: EngineProxy, maxDepth: number, trackKey: string,
      private trackIds: number[], namespace?: string) {
    super(maxDepth, trackKey, 'actual_frame_timeline_slice', namespace);
  }

  async onBoundsChange(start: time, end: time, resolution: duration):
      Promise<SliceData> {
    if (this.maxDur === 0n) {
      const maxDurResult = await this.engine.query(`
    select
      max(iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts, dur))
        as maxDur
    from experimental_slice_layout
    where filter_track_ids = '${this.trackIds.join(',')}'
  `);
      this.maxDur = maxDurResult.firstRow({maxDur: LONG_NULL}).maxDur || 0n;
    }

    const rawResult = await this.engine.query(`
  SELECT
    (s.ts + ${resolution / 2n}) / ${resolution} * ${resolution} as tsq,
    s.ts as ts,
    max(iif(s.dur = -1, (SELECT end_ts FROM trace_bounds) - s.ts, s.dur))
        as dur,
    s.layout_depth as layoutDepth,
    s.name as name,
    s.id as id,
    s.dur = 0 as isInstant,
    s.dur = -1 as isIncomplete,
    CASE afs.jank_tag
      WHEN 'Self Jank' THEN '${RED_COLOR}'
      WHEN 'Other Jank' THEN '${YELLOW_COLOR}'
      WHEN 'Dropped Frame' THEN '${BLUE_COLOR}'
      WHEN 'Buffer Stuffing' THEN '${LIGHT_GREEN_COLOR}'
      WHEN 'SurfaceFlinger Stuffing' THEN '${LIGHT_GREEN_COLOR}'
      WHEN 'No Jank' THEN '${GREEN_COLOR}'
      ELSE '${PINK_COLOR}'
    END as color
  from experimental_slice_layout s
  join actual_frame_timeline_slice afs using(id)
  where
    filter_track_ids = '${this.trackIds.join(',')}' and
    s.ts >= ${start - this.maxDur} and
    s.ts <= ${end}
  group by tsq, s.layout_depth
  order by tsq, s.layout_depth
`);

    const numRows = rawResult.numRows();
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
      colors: new Uint16Array(numRows),
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

    const it = rawResult.iter({
      'tsq': LONG,
      'ts': LONG,
      'dur': LONG,
      'layoutDepth': NUM,
      'id': NUM,
      'name': STR,
      'isInstant': NUM,
      'isIncomplete': NUM,
      'color': STR,
    });
    for (let i = 0; it.valid(); i++, it.next()) {
      const startQ = it.tsq;
      const start = it.ts;
      const dur = it.dur;
      const end = start + dur;
      const minEnd = startQ + resolution;
      const endQ = BIMath.max(BIMath.quant(end, resolution), minEnd);

      slices.starts[i] = startQ;
      slices.ends[i] = endQ;
      slices.depths[i] = it.layoutDepth;
      slices.titles[i] = internString(it.name);
      slices.colors![i] = internString(it.color);
      slices.sliceIds[i] = it.id;
      slices.isInstant[i] = it.isInstant;
      slices.isIncomplete[i] = it.isIncomplete;
    }
    return slices;
  }
}

class ActualFrames implements Plugin {
  onActivate(_ctx: PluginContext): void {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const {engine} = ctx;
    const result = await engine.query(`
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
        where process_track.name = "Actual Timeline"
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

      const kind = 'ActualFrames';
      const displayName =
          getTrackName({name: trackName, upid, pid, processName, kind});

      ctx.registerStaticTrack({
        uri: `perfetto.ActualFrames#${upid}`,
        displayName,
        trackIds,
        kind: ACTUAL_FRAMES_SLICE_TRACK_KIND,
        track: ({trackKey}) => {
          return new SliceTrack(
              engine,
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
  pluginId: 'perfetto.ActualFrames',
  plugin: ActualFrames,
};
