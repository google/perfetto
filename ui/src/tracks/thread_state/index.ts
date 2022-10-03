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

import {search} from '../../base/binary_search';
import {assertFalse} from '../../base/logging';
import {Actions} from '../../common/actions';
import {cropText} from '../../common/canvas_utils';
import {colorForState} from '../../common/colorizer';
import {PluginContext} from '../../common/plugin_api';
import {NUM, NUM_NULL, STR_NULL} from '../../common/query_result';
import {translateState} from '../../common/thread_state';
import {fromNs, toNs} from '../../common/time';
import {TrackData} from '../../common/track_data';
import {TrackController} from '../../controller/track_controller';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {NewTrackArgs, Track} from '../../frontend/track';


export const THREAD_STATE_TRACK_KIND = 'ThreadStateTrack';

export interface Data extends TrackData {
  strings: string[];
  ids: Float64Array;
  starts: Float64Array;
  ends: Float64Array;
  cpu: Int8Array;
  state: Uint16Array;  // Index into |strings|.
}

export interface Config {
  utid: number;
}

class ThreadStateTrackController extends TrackController<Config, Data> {
  static readonly kind = THREAD_STATE_TRACK_KIND;

  private maxDurNs = 0;

  async onSetup() {
    await this.query(`
      create view ${this.tableName('thread_state')} as
      select
        id,
        ts,
        dur,
        cpu,
        state,
        io_wait as ioWait
      from thread_state
      where utid = ${this.config.utid} and utid != 0
    `);

    const queryRes = await this.query(`
      select ifnull(max(dur), 0) as maxDur
      from ${this.tableName('thread_state')}
    `);
    this.maxDurNs = queryRes.firstRow({maxDur: NUM}).maxDur;
  }

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const resolutionNs = toNs(resolution);
    const startNs = toNs(start);
    const endNs = toNs(end);

    // ns per quantization bucket (i.e. ns per pixel). /2 * 2 is to force it to
    // be an even number, so we can snap in the middle.
    const bucketNs =
        Math.max(Math.round(resolutionNs * this.pxSize() / 2) * 2, 1);

    const query = `
      select
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as tsq,
        ts,
        state = 'S' as is_sleep,
        max(dur) as dur,
        ifnull(cast(cpu as integer), -1) as cpu,
        state,
        ioWait,
        ifnull(id, -1) as id
      from ${this.tableName('thread_state')}
      where
        ts >= ${startNs - this.maxDurNs} and
        ts <= ${endNs}
      group by tsq, is_sleep
      order by tsq
    `;

    const queryRes = await this.query(query);
    const numRows = queryRes.numRows();

    const data: Data = {
      start,
      end,
      resolution,
      length: numRows,
      ids: new Float64Array(numRows),
      starts: new Float64Array(numRows),
      ends: new Float64Array(numRows),
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
      'tsq': NUM,
      'ts': NUM,
      'dur': NUM,
      'cpu': NUM,
      'state': STR_NULL,
      'ioWait': NUM_NULL,
      'id': NUM,
    });
    for (let row = 0; it.valid(); it.next(), row++) {
      const startNsQ = it.tsq;
      const startNs = it.ts;
      const durNs = it.dur;
      const endNs = startNs + durNs;

      let endNsQ = Math.floor((endNs + bucketNs / 2 - 1) / bucketNs) * bucketNs;
      endNsQ = Math.max(endNsQ, startNsQ + bucketNs);

      const cpu = it.cpu;
      const state = it.state || undefined;
      const ioWait = it.ioWait === null ? undefined : !!it.ioWait;
      const id = it.id;

      // We should never have the end timestamp being the same as the bucket
      // start.
      assertFalse(startNsQ === endNsQ);

      data.starts[row] = fromNs(startNsQ);
      data.ends[row] = fromNs(endNsQ);
      data.state[row] = internState(state, ioWait);
      data.ids[row] = id;
      data.cpu[row] = cpu;
    }
    return data;
  }

  async onDestroy() {
    await this.query(`drop view if exists ${this.tableName('thread_state')}`);
  }
}

const MARGIN_TOP = 4;
const RECT_HEIGHT = 14;
const EXCESS_WIDTH = 10;

class ThreadStateTrack extends Track<Config, Data> {
  static readonly kind = THREAD_STATE_TRACK_KIND;
  static create(args: NewTrackArgs): ThreadStateTrack {
    return new ThreadStateTrack(args);
  }

  constructor(args: NewTrackArgs) {
    super(args);
  }

  getHeight(): number {
    return 2 * MARGIN_TOP + RECT_HEIGHT;
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    const {timeScale, visibleWindowTime} = globals.frontendLocalState;
    const data = this.data();
    const charWidth = ctx.measureText('dbpqaouk').width / 8;

    if (data === undefined) return;  // Can't possibly draw anything.

    // The draw of the rect on the selected slice must happen after the other
    // drawings, otherwise it would result under another rect.
    let drawRectOnSelected = () => {};

    checkerboardExcept(
        ctx,
        this.getHeight(),
        timeScale.timeToPx(visibleWindowTime.start),
        timeScale.timeToPx(visibleWindowTime.end),
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
      const tStart = data.starts[i];
      const tEnd = data.ends[i];
      const state = data.strings[data.state[i]];
      if (tEnd <= visibleWindowTime.start || tStart >= visibleWindowTime.end) {
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

      const color = colorForState(state);

      let colorStr = `hsl(${color.h},${color.s}%,${color.l}%)`;
      if (color.a) {
        colorStr = `hsla(${color.h},${color.s}%,${color.l}%, ${color.a})`;
      }
      ctx.fillStyle = colorStr;

      ctx.fillRect(rectStart, MARGIN_TOP, rectWidth, RECT_HEIGHT);

      // Don't render text when we have less than 10px to play with.
      if (rectWidth < 10 || state === 'Sleeping') continue;
      const title = cropText(state, charWidth, rectWidth);
      const rectXCenter = rectStart + rectWidth / 2;
      ctx.fillStyle = color.l > 80 ? '#404040' : '#fff';
      ctx.fillText(title, rectXCenter, MARGIN_TOP + RECT_HEIGHT / 2 + 3);

      if (isSelected) {
        drawRectOnSelected = () => {
          const rectStart =
              Math.max(0 - EXCESS_WIDTH, timeScale.timeToPx(tStart));
          const rectEnd = Math.min(
              timeScale.timeToPx(visibleWindowTime.end) + EXCESS_WIDTH,
              timeScale.timeToPx(tEnd));
          const color = colorForState(state);
          ctx.strokeStyle = `hsl(${color.h},${color.s}%,${color.l * 0.7}%)`;
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
    const data = this.data();
    if (data === undefined) return false;
    const {timeScale} = globals.frontendLocalState;
    const time = timeScale.pxToTime(x);
    const index = search(data.starts, time);
    if (index === -1) return false;

    const id = data.ids[index];
    globals.makeSelection(
        Actions.selectThreadState({id, trackId: this.trackState.id}));
    return true;
  }
}

function activate(ctx: PluginContext) {
  ctx.registerTrack(ThreadStateTrack);
  ctx.registerTrackController(ThreadStateTrackController);
}

export const plugin = {
  pluginId: 'perfetto.ThreadState',
  activate,
};
