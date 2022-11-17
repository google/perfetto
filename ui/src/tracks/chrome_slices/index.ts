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

import {Actions} from '../../common/actions';
import {cropText, drawIncompleteSlice} from '../../common/canvas_utils';
import {colorForThreadIdleSlice, hslForSlice} from '../../common/colorizer';
import {PluginContext} from '../../common/plugin_api';
import {NUM, NUM_NULL, STR} from '../../common/query_result';
import {fromNs, toNs} from '../../common/time';
import {TrackData} from '../../common/track_data';
import {
  TrackController,
} from '../../controller/track_controller';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {cachedHsluvToHex} from '../../frontend/hsluv_cache';
import {NewTrackArgs, SliceRect, Track} from '../../frontend/track';

export const SLICE_TRACK_KIND = 'ChromeSliceTrack';
const SLICE_HEIGHT = 18;
const TRACK_PADDING = 4;
const CHEVRON_WIDTH_PX = 10;
const HALF_CHEVRON_WIDTH_PX = CHEVRON_WIDTH_PX / 2;
const INNER_CHEVRON_OFFSET = -3;
const INNER_CHEVRON_SCALE =
    (SLICE_HEIGHT - 2 * INNER_CHEVRON_OFFSET) / SLICE_HEIGHT;
// the lowest bucketNs gets is 2, but add some room in case of fp error
const MIN_QUANT_NS = 3;

export interface Config {
  maxDepth: number;
  namespace: string;
  trackId: number;
}

export interface Data extends TrackData {
  // Slices are stored in a columnar fashion.
  strings: string[];
  sliceIds: Float64Array;
  starts: Float64Array;
  ends: Float64Array;
  depths: Uint16Array;
  titles: Uint16Array;   // Index into strings.
  colors?: Uint16Array;  // Index into strings.
  isInstant: Uint16Array;
  isIncomplete: Uint16Array;
  cpuTimeRatio?: Float64Array;
}

export class ChromeSliceTrackController extends TrackController<Config, Data> {
  static kind = SLICE_TRACK_KIND;
  private maxDurNs = 0;

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const startNs = toNs(start);
    const endNs = toNs(end);

    const pxSize = this.pxSize();

    // ns per quantization bucket (i.e. ns per pixel). /2 * 2 is to force it to
    // be an even number, so we can snap in the middle.
    const bucketNs = Math.max(Math.round(resolution * 1e9 * pxSize / 2) * 2, 1);

    const tableName = this.namespaceTable('slice');

    if (this.maxDurNs === 0) {
      const query = `
          SELECT max(iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts, dur))
          AS maxDur FROM ${tableName} WHERE track_id = ${this.config.trackId}`;
      const queryRes = await this.query(query);
      this.maxDurNs = queryRes.firstRow({maxDur: NUM_NULL}).maxDur || 0;
    }

    // Buckets are always even and positive, don't quantize once we zoom to
    // nanosecond-scale, so that we can see exact sizes.
    let tsq = `ts`;
    if (bucketNs > MIN_QUANT_NS) {
      tsq = `(ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs}`;
    }

    const query = `
      SELECT
        ${tsq} as tsq,
        ts,
        max(iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts, dur)) as dur,
        depth,
        id as sliceId,
        ifnull(name, '[null]') as name,
        dur = 0 as isInstant,
        dur = -1 as isIncomplete,
        thread_dur as threadDur
      FROM ${tableName}
      WHERE track_id = ${this.config.trackId} AND
        ts >= (${startNs - this.maxDurNs}) AND
        ts <= ${endNs}
      GROUP BY depth, tsq`;
    const queryRes = await this.query(query);

    const numRows = queryRes.numRows();
    const slices: Data = {
      start,
      end,
      resolution,
      length: numRows,
      strings: [],
      sliceIds: new Float64Array(numRows),
      starts: new Float64Array(numRows),
      ends: new Float64Array(numRows),
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
      tsq: NUM,
      ts: NUM,
      dur: NUM,
      depth: NUM,
      sliceId: NUM,
      name: STR,
      isInstant: NUM,
      isIncomplete: NUM,
      threadDur: NUM_NULL,
    });
    for (let row = 0; it.valid(); it.next(), row++) {
      const startNsQ = it.tsq;
      const startNs = it.ts;
      const durNs = it.dur;
      const endNs = startNs + durNs;

      let endNsQ = endNs;
      if (bucketNs > MIN_QUANT_NS) {
        endNsQ = Math.floor((endNs + bucketNs / 2 - 1) / bucketNs) * bucketNs;
        endNsQ = Math.max(endNsQ, startNsQ + bucketNs);
      }

      let isInstant = it.isInstant;
      // Floating point rounding with large numbers of nanoseconds can mean
      // there isn't enough precision to distinguish the start and end of a very
      // short event so we just display the event as an instant when zoomed in
      // rather than fail completely if the start and end time are the same.
      if (startNsQ === endNsQ) {
        isInstant = 1;
      }

      slices.starts[row] = fromNs(startNsQ);
      slices.ends[row] = fromNs(endNsQ);
      slices.depths[row] = it.depth;
      slices.sliceIds[row] = it.sliceId;
      slices.titles[row] = internString(it.name);
      slices.isInstant[row] = isInstant;
      slices.isIncomplete[row] = it.isIncomplete;

      let cpuTimeRatio = 1;
      if (!isInstant && !it.isIncomplete && it.threadDur !== null) {
        // Rounding the CPU time ratio to two decimal places and ensuring
        // it is less than or equal to one, incase the thread duration exceeds
        // the total duration.
        cpuTimeRatio =
            Math.min(Math.round((it.threadDur / it.dur) * 100) / 100, 1);
      }
      slices.cpuTimeRatio![row] = cpuTimeRatio;
    }
    return slices;
  }
}

export class ChromeSliceTrack extends Track<Config, Data> {
  static readonly kind: string = SLICE_TRACK_KIND;
  static create(args: NewTrackArgs): Track {
    return new ChromeSliceTrack(args);
  }

  private hoveredTitleId = -1;

  constructor(args: NewTrackArgs) {
    super(args);
  }

  // Font used to render the slice name on the current track.
  protected getFont() {
    return '12px Roboto Condensed';
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    // TODO: fonts and colors should come from the CSS and not hardcoded here.

    const {timeScale, visibleWindowTime} = globals.frontendLocalState;
    const data = this.data();

    if (data === undefined) return;  // Can't possibly draw anything.

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
        ctx,
        this.getHeight(),
        timeScale.timeToPx(visibleWindowTime.start),
        timeScale.timeToPx(visibleWindowTime.end),
        timeScale.timeToPx(data.start),
        timeScale.timeToPx(data.end),
    );

    ctx.textAlign = 'center';

    // measuretext is expensive so we only use it once.
    const charWidth = ctx.measureText('ACBDLqsdfg').width / 10;

    // The draw of the rect on the selected slice must happen after the other
    // drawings, otherwise it would result under another rect.
    let drawRectOnSelected = () => {};

    for (let i = 0; i < data.starts.length; i++) {
      const tStart = data.starts[i];
      let tEnd = data.ends[i];
      const depth = data.depths[i];
      const titleId = data.titles[i];
      const sliceId = data.sliceIds[i];
      const isInstant = data.isInstant[i];
      const isIncomplete = data.isIncomplete[i];
      const title = data.strings[titleId];
      const colorOverride = data.colors && data.strings[data.colors[i]];
      if (isIncomplete) {  // incomplete slice
        tEnd = visibleWindowTime.end;
      }

      const rect = this.getSliceRect(tStart, tEnd, depth);
      if (!rect || !rect.visible) {
        continue;
      }

      const currentSelection = globals.state.currentSelection;
      const isSelected = currentSelection &&
          currentSelection.kind === 'CHROME_SLICE' &&
          currentSelection.id !== undefined && currentSelection.id === sliceId;

      const name = title.replace(/( )?\d+/g, '');
      const highlighted = titleId === this.hoveredTitleId ||
          globals.state.highlightedSliceId === sliceId;

      const hasFocus = highlighted || isSelected;

      const [hue, saturation, lightness] = hslForSlice(name, hasFocus);

      let color: string;
      if (colorOverride === undefined) {
        color = cachedHsluvToHex(hue, saturation, lightness);
      } else {
        color = colorOverride;
      }
      ctx.fillStyle = color;

      // We draw instant events as upward facing chevrons starting at A:
      //     A
      //    ###
      //   ##C##
      //  ##   ##
      // D       B
      // Then B, C, D and back to A:
      if (isInstant) {
        if (isSelected) {
          drawRectOnSelected = () => {
            ctx.save();
            ctx.translate(rect.left, rect.top);

            // Draw outer chevron as dark border
            ctx.save();
            ctx.translate(0, INNER_CHEVRON_OFFSET);
            ctx.scale(INNER_CHEVRON_SCALE, INNER_CHEVRON_SCALE);
            ctx.fillStyle = cachedHsluvToHex(hue, 100, 10);

            this.drawChevron(ctx);
            ctx.restore();

            // Draw inner chevron as interior
            ctx.fillStyle = color;
            this.drawChevron(ctx);

            ctx.restore();
          };
        } else {
          ctx.save();
          ctx.translate(rect.left, rect.top);
          this.drawChevron(ctx);
          ctx.restore();
        }
        continue;
      }

      if (isIncomplete && rect.width > SLICE_HEIGHT / 4) {
        drawIncompleteSlice(ctx, rect.left, rect.top, rect.width, SLICE_HEIGHT);
      } else if (
          data.cpuTimeRatio !== undefined && data.cpuTimeRatio[i] < 1 - 1e-9) {
        // We draw two rectangles, representing the ratio between wall time and
        // time spent on cpu.
        const cpuTimeRatio = data.cpuTimeRatio![i];
        const firstPartWidth = rect.width * cpuTimeRatio;
        const secondPartWidth = rect.width * (1 - cpuTimeRatio);
        ctx.fillRect(rect.left, rect.top, firstPartWidth, SLICE_HEIGHT);
        ctx.fillStyle =
            colorForThreadIdleSlice(hue, saturation, lightness, hasFocus);
        ctx.fillRect(
            rect.left + firstPartWidth,
            rect.top,
            secondPartWidth,
            SLICE_HEIGHT);
      } else {
        ctx.fillRect(rect.left, rect.top, rect.width, SLICE_HEIGHT);
      }

      // Selected case
      if (isSelected) {
        drawRectOnSelected = () => {
          ctx.strokeStyle = cachedHsluvToHex(hue, 100, 10);
          ctx.beginPath();
          ctx.lineWidth = 3;
          ctx.strokeRect(
              rect.left, rect.top - 1.5, rect.width, SLICE_HEIGHT + 3);
          ctx.closePath();
        };
      }

      ctx.fillStyle = lightness > 65 ? '#404040' : 'white';
      const displayText = cropText(title, charWidth, rect.width);
      const rectXCenter = rect.left + rect.width / 2;
      ctx.textBaseline = 'middle';
      ctx.font = this.getFont();
      ctx.fillText(displayText, rectXCenter, rect.top + SLICE_HEIGHT / 2);
    }
    drawRectOnSelected();
  }

  drawChevron(ctx: CanvasRenderingContext2D) {
    // Draw a chevron at a fixed location and size. Should be used with
    // ctx.translate and ctx.scale to alter location and size.
    ctx.beginPath();
    ctx.moveTo(0, 0);
    ctx.lineTo(HALF_CHEVRON_WIDTH_PX, SLICE_HEIGHT);
    ctx.lineTo(0, SLICE_HEIGHT - HALF_CHEVRON_WIDTH_PX);
    ctx.lineTo(-HALF_CHEVRON_WIDTH_PX, SLICE_HEIGHT);
    ctx.lineTo(0, 0);
    ctx.fill();
  }

  getSliceIndex({x, y}: {x: number, y: number}): number|void {
    const data = this.data();
    if (data === undefined) return;
    const {timeScale} = globals.frontendLocalState;
    if (y < TRACK_PADDING) return;
    const instantWidthTime = timeScale.deltaPxToDuration(HALF_CHEVRON_WIDTH_PX);
    const t = timeScale.pxToTime(x);
    const depth = Math.floor((y - TRACK_PADDING) / SLICE_HEIGHT);
    for (let i = 0; i < data.starts.length; i++) {
      if (depth !== data.depths[i]) {
        continue;
      }
      const tStart = data.starts[i];
      if (data.isInstant[i]) {
        if (Math.abs(tStart - t) < instantWidthTime) {
          return i;
        }
      } else {
        let tEnd = data.ends[i];
        if (data.isIncomplete[i]) {
          tEnd = globals.frontendLocalState.visibleWindowTime.end;
        }
        if (tStart <= t && t <= tEnd) {
          return i;
        }
      }
    }
  }

  onMouseMove({x, y}: {x: number, y: number}) {
    this.hoveredTitleId = -1;
    globals.dispatch(Actions.setHighlightedSliceId({sliceId: -1}));
    const sliceIndex = this.getSliceIndex({x, y});
    if (sliceIndex === undefined) return;
    const data = this.data();
    if (data === undefined) return;
    this.hoveredTitleId = data.titles[sliceIndex];
    const sliceId = data.sliceIds[sliceIndex];
    globals.dispatch(Actions.setHighlightedSliceId({sliceId}));
  }

  onMouseOut() {
    this.hoveredTitleId = -1;
    globals.dispatch(Actions.setHighlightedSliceId({sliceId: -1}));
  }

  onMouseClick({x, y}: {x: number, y: number}): boolean {
    const sliceIndex = this.getSliceIndex({x, y});
    if (sliceIndex === undefined) return false;
    const data = this.data();
    if (data === undefined) return false;
    const sliceId = data.sliceIds[sliceIndex];
    if (sliceId !== undefined && sliceId !== -1) {
      globals.makeSelection(Actions.selectChromeSlice({
        id: sliceId,
        trackId: this.trackState.id,
        table: this.config.namespace,
      }));
      return true;
    }
    return false;
  }

  getHeight() {
    return SLICE_HEIGHT * (this.config.maxDepth + 1) + 2 * TRACK_PADDING;
  }

  getSliceRect(tStart: number, tEnd: number, depth: number): SliceRect
      |undefined {
    const {timeScale, visibleWindowTime} = globals.frontendLocalState;
    const pxEnd = timeScale.timeToPx(visibleWindowTime.end);
    const left = Math.max(timeScale.timeToPx(tStart), 0);
    const right = Math.min(timeScale.timeToPx(tEnd), pxEnd);
    return {
      left,
      width: Math.max(right - left, 1),
      top: TRACK_PADDING + depth * SLICE_HEIGHT,
      height: SLICE_HEIGHT,
      visible:
          !(tEnd <= visibleWindowTime.start || tStart >= visibleWindowTime.end),
    };
  }
}

function activate(ctx: PluginContext) {
  ctx.registerTrackController(ChromeSliceTrackController);
  ctx.registerTrack(ChromeSliceTrack);
}

export const plugin = {
  pluginId: 'perfetto.ChromeSlices',
  activate,
};
