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

import {v4 as uuidv4} from 'uuid';

import {BigintMath as BIMath} from '../../base/bigint_math';
import {search} from '../../base/binary_search';
import {assertFalse} from '../../base/logging';
import {duration, Time, time} from '../../base/time';
import {Actions} from '../../common/actions';
import {cropText} from '../../common/canvas_utils';
import {colorForState} from '../../common/colorizer';
import {translateState} from '../../common/thread_state';
import {TrackData} from '../../common/track_data';
import {TimelineFetcher} from '../../common/track_helper';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {PanelSize} from '../../frontend/panel';
import {
  EngineProxy,
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
  Track,
} from '../../public';
import {getTrackName} from '../../public/utils';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';

import {
  ThreadStateTrack as ThreadStateTrackV2,
} from './thread_state_v2';

export const THREAD_STATE_TRACK_KIND = 'ThreadStateTrack';

interface Data extends TrackData {
  strings: string[];
  ids: Float64Array;
  starts: BigInt64Array;
  ends: BigInt64Array;
  cpu: Int8Array;
  state: Uint16Array;  // Index into |strings|.
}

const MARGIN_TOP = 3;
const RECT_HEIGHT = 12;
const EXCESS_WIDTH = 10;

class ThreadStateTrack implements Track {
  private fetcher = new TimelineFetcher<Data>(this.onBoundsChange.bind(this));
  private trackKey: string;
  private engine: EngineProxy;
  private utid: number;
  private uuid = uuidv4();

  constructor(trackKey: string, engine: EngineProxy, utid: number) {
    this.trackKey = trackKey;
    this.engine = engine;
    this.utid = utid;
  }

  private maxDurNs: duration = 0n;

  // Returns a valid SQL table name with the given prefix that should be unique
  // for each track.
  private tableName(prefix: string) {
    // Derive table name from, since that is unique for each track.
    // Track ID can be UUID but '-' is not valid for sql table name.
    const idSuffix = this.uuid.split('-').join('_');
    return `${prefix}_${idSuffix}`;
  }

  async onCreate() {
    await this.engine.query(`
      create view ${this.tableName('thread_state')} as
      select
        id,
        ts,
        dur,
        cpu,
        state,
        io_wait as ioWait
      from thread_state
      where utid = ${this.utid} and utid != 0
    `);

    const queryRes = await this.engine.query(`
      select ifnull(max(dur), 0) as maxDur
      from ${this.tableName('thread_state')}
    `);
    this.maxDurNs = queryRes.firstRow({maxDur: LONG}).maxDur;
  }

  async onUpdate() {
    await this.fetcher.requestDataForCurrentTime();
  }

  async onBoundsChange(start: time, end: time, resolution: duration):
      Promise<Data> {
    const query = `
      select
        (ts + ${resolution / 2n}) / ${resolution} * ${resolution} as tsq,
        ts,
        state = 'S' as is_sleep,
        max(dur) as dur,
        ifnull(cast(cpu as integer), -1) as cpu,
        state,
        ioWait,
        ifnull(id, -1) as id
      from ${this.tableName('thread_state')}
      where
        ts >= ${start - this.maxDurNs} and
        ts <= ${end}
      group by tsq, is_sleep
      order by tsq
    `;

    const queryRes = await this.engine.query(query);
    const numRows = queryRes.numRows();

    const data: Data = {
      start,
      end,
      resolution,
      length: numRows,
      ids: new Float64Array(numRows),
      starts: new BigInt64Array(numRows),
      ends: new BigInt64Array(numRows),
      strings: [],
      state: new Uint16Array(numRows),
      cpu: new Int8Array(numRows),
    };

    const stringIndexes = new Map<
        {shortState: string | undefined; ioWait: boolean | undefined},
        number>();
    function internState(
        shortState: string|undefined, ioWait: boolean|undefined) {
      let idx = stringIndexes.get({shortState, ioWait});
      if (idx !== undefined) return idx;
      idx = data.strings.length;
      data.strings.push(translateState(shortState, ioWait));
      stringIndexes.set({shortState, ioWait}, idx);
      return idx;
    }
    const it = queryRes.iter({
      'tsq': LONG,
      'ts': LONG,
      'dur': LONG,
      'cpu': NUM,
      'state': STR_NULL,
      'ioWait': NUM_NULL,
      'id': NUM,
    });
    for (let row = 0; it.valid(); it.next(), row++) {
      const startQ = it.tsq;
      const start = it.ts;
      const dur = it.dur;
      const end = start + dur;
      const minEnd = startQ + resolution;
      const endQ = BIMath.max(BIMath.quant(end, resolution), minEnd);

      const cpu = it.cpu;
      const state = it.state || undefined;
      const ioWait = it.ioWait === null ? undefined : !!it.ioWait;
      const id = it.id;

      // We should never have the end timestamp being the same as the bucket
      // start.
      assertFalse(startQ === endQ);

      data.starts[row] = startQ;
      data.ends[row] = endQ;
      data.state[row] = internState(state, ioWait);
      data.ids[row] = id;
      data.cpu[row] = cpu;
    }
    return data;
  }

  async onDestroy() {
    if (this.engine.isAlive) {
      await this.engine.query(
          `drop view if exists ${this.tableName('thread_state')}`);
    }
    this.fetcher.dispose();
  }

  getHeight(): number {
    return 2 * MARGIN_TOP + RECT_HEIGHT;
  }

  render(ctx: CanvasRenderingContext2D, size: PanelSize): void {
    const {
      visibleTimeScale: timeScale,
      visibleTimeSpan,
    } = globals.timeline;
    const data = this.fetcher.data;
    const charWidth = ctx.measureText('dbpqaouk').width / 8;

    if (data === undefined) return;  // Can't possibly draw anything.

    // The draw of the rect on the selected slice must happen after the other
    // drawings, otherwise it would result under another rect.
    let drawRectOnSelected = () => {};

    checkerboardExcept(
        ctx,
        this.getHeight(),
        0,
        size.width,
        timeScale.timeToPx(data.start),
        timeScale.timeToPx(data.end),
    );

    ctx.textAlign = 'center';
    ctx.font = '10px Roboto Condensed';

    for (let i = 0; i < data.starts.length; i++) {
      // NOTE: Unlike userspace and scheduling slices, thread state slices are
      // allowed to overlap; specifically, sleeping slices are allowed to
      // overlap with non-sleeping slices. We do this because otherwise
      // sleeping slices generally dominate traces making it seem like there are
      // no running/runnable etc. slices until you zoom in. By drawing both,
      // we get a more accurate representation of the trace and prevent weird
      // artifacts when zooming.
      // See b/201793731 for an example of why we do this.
      const tStart = Time.fromRaw(data.starts[i]);
      const tEnd = Time.fromRaw(data.ends[i]);
      const state = data.strings[data.state[i]];
      if (!visibleTimeSpan.intersects(tStart, tEnd)) {
        continue;
      }

      // Don't display a slice for Task Dead.
      if (state === 'x') continue;
      const rectStart = timeScale.timeToPx(tStart);
      const rectEnd = timeScale.timeToPx(tEnd);
      const rectWidth = rectEnd - rectStart;

      const currentSelection = globals.state.currentSelection;
      const isSelected = currentSelection &&
          currentSelection.kind === 'THREAD_STATE' &&
          currentSelection.id === data.ids[i];

      const colorScheme = colorForState(state);
      ctx.fillStyle = colorScheme.base.cssString;
      ctx.fillRect(rectStart, MARGIN_TOP, rectWidth, RECT_HEIGHT);

      // Don't render text when we have less than 10px to play with.
      if (rectWidth < 10 || state === 'Sleeping') continue;
      const title = cropText(state, charWidth, rectWidth);
      const rectXCenter = rectStart + rectWidth / 2;
      ctx.fillStyle = colorScheme.textBase.cssString;
      ctx.fillText(title, rectXCenter, MARGIN_TOP + RECT_HEIGHT / 2 + 3);

      if (isSelected) {
        drawRectOnSelected = () => {
          const rectStart =
              Math.max(0 - EXCESS_WIDTH, timeScale.timeToPx(tStart));
          const rectEnd =
              Math.min(size.width + EXCESS_WIDTH, timeScale.timeToPx(tEnd));
          ctx.strokeStyle = colorScheme.base.cssString;
          ctx.beginPath();
          ctx.lineWidth = 3;
          ctx.strokeRect(
              rectStart,
              MARGIN_TOP - 1.5,
              rectEnd - rectStart,
              RECT_HEIGHT + 3);
          ctx.closePath();
        };
      }
    }
    drawRectOnSelected();
  }

  onMouseClick({x}: {x: number}) {
    const data = this.fetcher.data;
    if (data === undefined) return false;
    const {visibleTimeScale} = globals.timeline;
    const time = visibleTimeScale.pxToHpTime(x);
    const index = search(data.starts, time.toTime());
    if (index === -1) return false;
    const id = data.ids[index];
    globals.makeSelection(
        Actions.selectThreadState({id, trackKey: this.trackKey}));
    return true;
  }
}


class ThreadState implements Plugin {
  onActivate(_ctx: PluginContext): void {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const {engine} = ctx;
    const result = await engine.query(`
      select
        utid,
        upid,
        tid,
        pid,
        thread.name as threadName
      from
        thread_state
        left join thread using(utid)
        left join process using(upid)
      where utid != 0
      group by utid`);

    const it = result.iter({
      utid: NUM,
      upid: NUM_NULL,
      tid: NUM_NULL,
      pid: NUM_NULL,
      threadName: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const upid = it.upid;
      const tid = it.tid;
      const threadName = it.threadName;
      const displayName =
          getTrackName({utid, tid, threadName, kind: THREAD_STATE_TRACK_KIND});

      ctx.registerTrack({
        uri: `perfetto.ThreadState#${upid}.${utid}`,
        displayName,
        kind: THREAD_STATE_TRACK_KIND,
        utid: utid,
        track: ({trackKey}) => {
          return new ThreadStateTrack(trackKey, ctx.engine, utid);
        },
      });

      ctx.registerTrack({
        uri: `perfetto.ThreadState#${utid}.v2`,
        displayName,
        kind: THREAD_STATE_TRACK_KIND,
        utid,
        track: ({trackKey}) => {
          return new ThreadStateTrackV2(
              {
                engine: ctx.engine,
                trackKey,
              },
              utid);
        },
      });
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.ThreadState',
  plugin: ThreadState,
};
