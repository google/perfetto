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
import {clamp} from '../../base/math_utils';
import {Duration, duration, time} from '../../base/time';
import {uuidv4} from '../../base/uuid';
import {ChromeSliceDetailsTab} from '../../frontend/chrome_slice_details_tab';
import {
  NAMED_ROW,
  NamedSliceTrack,
  NamedSliceTrackTypes,
} from '../../frontend/named_slice_track';
import {SLICE_LAYOUT_FIT_CONTENT_DEFAULTS} from '../../frontend/slice_layout';
import {
  SliceData,
  SliceTrackLEGACY,
} from '../../frontend/slice_track';
import {NewTrackArgs} from '../../frontend/track';
import {
  BottomTabToSCSAdapter,
  EngineProxy,
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {getTrackName} from '../../public/utils';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';

export const SLICE_TRACK_KIND = 'ChromeSliceTrack';

export class ChromeSliceTrack extends SliceTrackLEGACY {
  private maxDurNs: duration = 0n;

  constructor(
      protected engine: EngineProxy, maxDepth: number, trackKey: string,
      private trackId: number, namespace?: string) {
    super(maxDepth, trackKey, 'slice', namespace);
  }

  async onBoundsChange(start: time, end: time, resolution: duration):
      Promise<SliceData> {
    const tableName = this.namespaceTable('slice');

    if (this.maxDurNs === Duration.ZERO) {
      const query = `
          SELECT max(iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts, dur))
          AS maxDur FROM ${tableName} WHERE track_id = ${this.trackId}`;
      const queryRes = await this.engine.query(query);
      this.maxDurNs = queryRes.firstRow({maxDur: LONG_NULL}).maxDur ?? 0n;
    }

    const query = `
      SELECT
        (ts + ${resolution / 2n}) / ${resolution} * ${resolution} as tsq,
        ts,
        max(iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts, dur)) as dur,
        depth,
        id as sliceId,
        ifnull(name, '[null]') as name,
        dur = 0 as isInstant,
        dur = -1 as isIncomplete,
        thread_dur as threadDur
      FROM ${tableName}
      WHERE track_id = ${this.trackId} AND
        ts >= (${start - this.maxDurNs}) AND
        ts <= ${end}
      GROUP BY depth, tsq`;
    const queryRes = await this.engine.query(query);

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
      cpuTimeRatio: new Float64Array(numRows),
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
      sliceId: NUM,
      name: STR,
      isInstant: NUM,
      isIncomplete: NUM,
      threadDur: LONG_NULL,
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
      slices.sliceIds[row] = it.sliceId;
      slices.titles[row] = internString(it.name);
      slices.isInstant[row] = it.isInstant;
      slices.isIncomplete[row] = it.isIncomplete;

      let cpuTimeRatio = 1;
      if (!it.isInstant && !it.isIncomplete && it.threadDur !== null) {
        // Rounding the CPU time ratio to two decimal places and ensuring
        // it is less than or equal to one, incase the thread duration exceeds
        // the total duration.
        cpuTimeRatio = Math.min(
          Math.round(BIMath.ratio(it.threadDur, it.dur) * 100) / 100, 1);
      }
      slices.cpuTimeRatio![row] = cpuTimeRatio;
    }
    return slices;
  }
}

export const CHROME_SLICE_ROW = {
  // Base columns (tsq, ts, dur, id, depth).
  ...NAMED_ROW,

  // Chrome-specific columns.
  threadDur: LONG_NULL,
};
export type ChromeSliceRow = typeof CHROME_SLICE_ROW;

export interface ChromeSliceTrackTypes extends NamedSliceTrackTypes {
  row: ChromeSliceRow;
}

export class ChromeSliceTrackV2 extends NamedSliceTrack<ChromeSliceTrackTypes> {
  constructor(args: NewTrackArgs, private trackId: number, maxDepth: number) {
    super(args);
    this.sliceLayout = {
      ...SLICE_LAYOUT_FIT_CONTENT_DEFAULTS,
      depthGuess: maxDepth,
    };
  }

  // This is used by the base class to call iter().
  getRowSpec() {
    return CHROME_SLICE_ROW;
  }

  getSqlSource(): string {
    return `select
      ts,
      dur,
      id,
      depth,
      ifnull(name, '') as name,
      thread_dur as threadDur
    from slice
    where track_id = ${this.trackId}`;
  }

  // Converts a SQL result row to an "Impl" Slice.
  rowToSlice(row: ChromeSliceTrackTypes['row']):
      ChromeSliceTrackTypes['slice'] {
    const namedSlice = super.rowToSlice(row);

    if (row.dur > 0n && row.threadDur !== null) {
      const fillRatio = clamp(BIMath.ratio(row.threadDur, row.dur), 0, 1);
      return {...namedSlice, fillRatio};
    } else {
      return namedSlice;
    }
  }

  onUpdatedSlices(slices: ChromeSliceTrackTypes['slice'][]) {
    for (const slice of slices) {
      slice.isHighlighted = (slice === this.hoveredSlice);
    }
  }
}

class ChromeSlicesPlugin implements Plugin {
  onActivate(_ctx: PluginContext): void {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const {engine} = ctx;
    const result = await engine.query(`
        select
          thread_track.utid as utid,
          thread_track.id as trackId,
          thread_track.name as trackName,
          EXTRACT_ARG(thread_track.source_arg_set_id,
                      'is_root_in_scope') as isDefaultTrackForScope,
          tid,
          thread.name as threadName,
          max(slice.depth) as maxDepth,
          process.upid as upid
        from slice
        join thread_track on slice.track_id = thread_track.id
        join thread using(utid)
        left join process using(upid)
        group by thread_track.id
  `);

    const it = result.iter({
      utid: NUM,
      trackId: NUM,
      trackName: STR_NULL,
      isDefaultTrackForScope: NUM_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
      maxDepth: NUM,
      upid: NUM_NULL,
    });

    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const trackId = it.trackId;
      const trackName = it.trackName;
      const tid = it.tid;
      const threadName = it.threadName;
      const maxDepth = it.maxDepth;

      const displayName = getTrackName({
        name: trackName,
        utid,
        tid,
        threadName,
        kind: 'Slices',
      });

      ctx.registerTrack({
        uri: `perfetto.ChromeSlices#${trackId}`,
        displayName,
        trackIds: [trackId],
        kind: SLICE_TRACK_KIND,
        trackFactory: ({trackKey}) => {
          const newTrackArgs = {
            engine: ctx.engine,
            trackKey,
          };
          return new ChromeSliceTrackV2(newTrackArgs, trackId, maxDepth);
        },
      });
    }

    ctx.registerDetailsPanel(new BottomTabToSCSAdapter({
      tabFactory: (sel) => {
        if (sel.kind !== 'CHROME_SLICE') {
          return undefined;
        }
        return new ChromeSliceDetailsTab({
          config: {
            table: sel.table ?? 'slice',
            id: sel.id,
          },
          engine: ctx.engine,
          uuid: uuidv4(),
        });
      },
    }));
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.ChromeSlices',
  plugin: ChromeSlicesPlugin,
};
