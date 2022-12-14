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

import * as m from 'mithril';

import {searchSegment} from '../../base/binary_search';
import {assertTrue} from '../../base/logging';
import {Actions} from '../../common/actions';
import {
  EngineProxy,
  NUM,
  NUM_NULL,
  PluginContext,
  STR,
  TrackInfo,
} from '../../common/plugin_api';
import {fromNs, toNs} from '../../common/time';
import {TrackData} from '../../common/track_data';
import {
  TrackController,
} from '../../controller/track_controller';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {PopupMenuButton, PopupMenuItem} from '../../frontend/popup_menu';
import {NewTrackArgs, Track} from '../../frontend/track';

export const COUNTER_TRACK_KIND = 'CounterTrack';

// TODO(hjd): Convert to enum.
export type CounterScaleOptions =
    'ZERO_BASED'|'MIN_MAX'|'DELTA_FROM_PREVIOUS'|'RATE';

export interface Data extends TrackData {
  maximumValue: number;
  minimumValue: number;
  maximumDelta: number;
  minimumDelta: number;
  maximumRate: number;
  minimumRate: number;
  timestamps: Float64Array;
  lastIds: Float64Array;
  minValues: Float64Array;
  maxValues: Float64Array;
  lastValues: Float64Array;
  totalDeltas: Float64Array;
  rate: Float64Array;
}

export interface Config {
  name: string;
  maximumValue?: number;
  minimumValue?: number;
  startTs?: number;
  endTs?: number;
  namespace: string;
  trackId: number;
  scale?: CounterScaleOptions;
}

class CounterTrackController extends TrackController<Config, Data> {
  static readonly kind = COUNTER_TRACK_KIND;
  private setup = false;
  private maximumValueSeen = 0;
  private minimumValueSeen = 0;
  private maximumDeltaSeen = 0;
  private minimumDeltaSeen = 0;
  private maxDurNs = 0;

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    const startNs = toNs(start);
    const endNs = toNs(end);

    const pxSize = this.pxSize();

    // ns per quantization bucket (i.e. ns per pixel). /2 * 2 is to force it to
    // be an even number, so we can snap in the middle.
    const bucketNs = Math.max(Math.round(resolution * 1e9 * pxSize / 2) * 2, 1);

    if (!this.setup) {
      if (this.config.namespace === undefined) {
        await this.query(`
          create view ${this.tableName('counter_view')} as
          select
            id,
            ts,
            dur,
            value,
            delta
          from experimental_counter_dur
          where track_id = ${this.config.trackId};
        `);
      } else {
        await this.query(`
          create view ${this.tableName('counter_view')} as
          select
            id,
            ts,
            lead(ts, 1, ts) over (order by ts) - ts as dur,
            lead(value, 1, value) over (order by ts) - value as delta,
            value
          from ${this.namespaceTable('counter')}
          where track_id = ${this.config.trackId};
        `);
      }

      const maxDurResult = await this.query(`
          select
            max(
              iif(dur != -1, dur, (select end_ts from trace_bounds) - ts)
            ) as maxDur
          from ${this.tableName('counter_view')}
      `);
      this.maxDurNs = maxDurResult.firstRow({maxDur: NUM_NULL}).maxDur || 0;

      const queryRes = await this.query(`
        select
          ifnull(max(value), 0) as maxValue,
          ifnull(min(value), 0) as minValue,
          ifnull(max(delta), 0) as maxDelta,
          ifnull(min(delta), 0) as minDelta
        from ${this.tableName('counter_view')}`);
      const row = queryRes.firstRow(
          {maxValue: NUM, minValue: NUM, maxDelta: NUM, minDelta: NUM});
      this.maximumValueSeen = row.maxValue;
      this.minimumValueSeen = row.minValue;
      this.maximumDeltaSeen = row.maxDelta;
      this.minimumDeltaSeen = row.minDelta;

      this.setup = true;
    }

    const queryRes = await this.query(`
      select
        (ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs} as tsq,
        min(value) as minValue,
        max(value) as maxValue,
        sum(delta) as totalDelta,
        value_at_max_ts(ts, id) as lastId,
        value_at_max_ts(ts, value) as lastValue
      from ${this.tableName('counter_view')}
      where ts >= ${startNs - this.maxDurNs} and ts <= ${endNs}
      group by tsq
      order by tsq
    `);

    const numRows = queryRes.numRows();

    const data: Data = {
      start,
      end,
      length: numRows,
      maximumValue: this.maximumValue(),
      minimumValue: this.minimumValue(),
      maximumDelta: this.maximumDeltaSeen,
      minimumDelta: this.minimumDeltaSeen,
      maximumRate: 0,
      minimumRate: 0,
      resolution,
      timestamps: new Float64Array(numRows),
      lastIds: new Float64Array(numRows),
      minValues: new Float64Array(numRows),
      maxValues: new Float64Array(numRows),
      lastValues: new Float64Array(numRows),
      totalDeltas: new Float64Array(numRows),
      rate: new Float64Array(numRows),
    };

    const it = queryRes.iter({
      'tsq': NUM,
      'lastId': NUM,
      'minValue': NUM,
      'maxValue': NUM,
      'lastValue': NUM,
      'totalDelta': NUM,
    });
    let lastValue = 0;
    let lastTs = 0;
    for (let row = 0; it.valid(); it.next(), row++) {
      const ts = fromNs(it.tsq);
      const value = it.lastValue;
      const rate = (value - lastValue) / (ts - lastTs);
      lastTs = ts;
      lastValue = value;

      data.timestamps[row] = ts;
      data.lastIds[row] = it.lastId;
      data.minValues[row] = it.minValue;
      data.maxValues[row] = it.maxValue;
      data.lastValues[row] = value;
      data.totalDeltas[row] = it.totalDelta;
      data.rate[row] = rate;
      if (row > 0) {
        data.rate[row - 1] = rate;
        data.maximumRate = Math.max(data.maximumRate, rate);
        data.minimumRate = Math.min(data.minimumRate, rate);
      }
    }
    return data;
  }

  private maximumValue() {
    if (this.config.maximumValue === undefined) {
      return this.maximumValueSeen;
    } else {
      return this.config.maximumValue;
    }
  }

  private minimumValue() {
    if (this.config.minimumValue === undefined) {
      return this.minimumValueSeen;
    } else {
      return this.config.minimumValue;
    }
  }
}


// 0.5 Makes the horizontal lines sharp.
const MARGIN_TOP = 3.5;
const RECT_HEIGHT = 24.5;

class CounterTrack extends Track<Config, Data> {
  static readonly kind = COUNTER_TRACK_KIND;
  static create(args: NewTrackArgs): CounterTrack {
    return new CounterTrack(args);
  }

  private mousePos = {x: 0, y: 0};
  private hoveredValue: number|undefined = undefined;
  private hoveredTs: number|undefined = undefined;
  private hoveredTsEnd: number|undefined = undefined;

  constructor(args: NewTrackArgs) {
    super(args);
  }

  getHeight() {
    return MARGIN_TOP + RECT_HEIGHT;
  }

  getContextMenu(): m.Vnode<any> {
    const currentScale = this.config.scale;
    const scales: {name: CounterScaleOptions, humanName: string}[] = [
      {name: 'ZERO_BASED', humanName: 'Zero based'},
      {name: 'MIN_MAX', humanName: 'Min/Max'},
      {name: 'DELTA_FROM_PREVIOUS', humanName: 'Delta'},
      {name: 'RATE', humanName: 'Rate'},
    ];
    const items: PopupMenuItem[] = [];
    for (const scale of scales) {
      let text;
      if (currentScale === scale.name) {
        text = `*${scale.humanName}*`;
      } else {
        text = scale.humanName;
      }
      items.push({
        itemType: 'regular',
        text,
        callback: () => {
          this.config.scale = scale.name;
          Actions.updateTrackConfig({
            id: this.trackState.id,
            config: this.config,
          });
        },
      });
    }
    return m(PopupMenuButton, {
      icon: 'show_chart',
      items,
    });
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    // TODO: fonts and colors should come from the CSS and not hardcoded here.
    const {timeScale, visibleWindowTime} = globals.frontendLocalState;
    const data = this.data();

    // Can't possibly draw anything.
    if (data === undefined || data.timestamps.length === 0) {
      return;
    }

    assertTrue(data.timestamps.length === data.minValues.length);
    assertTrue(data.timestamps.length === data.maxValues.length);
    assertTrue(data.timestamps.length === data.lastValues.length);
    assertTrue(data.timestamps.length === data.totalDeltas.length);
    assertTrue(data.timestamps.length === data.rate.length);

    const scale: CounterScaleOptions = this.config.scale || 'ZERO_BASED';

    let minValues = data.minValues;
    let maxValues = data.maxValues;
    let lastValues = data.lastValues;
    let maximumValue = data.maximumValue;
    let minimumValue = data.minimumValue;
    if (scale === 'DELTA_FROM_PREVIOUS') {
      lastValues = data.totalDeltas;
      minValues = data.totalDeltas;
      maxValues = data.totalDeltas;
      maximumValue = data.maximumDelta;
      minimumValue = data.minimumDelta;
    }
    if (scale === 'RATE') {
      lastValues = data.rate;
      minValues = data.rate;
      maxValues = data.rate;
      maximumValue = data.maximumRate;
      minimumValue = data.minimumRate;
    }

    const endPx = Math.floor(timeScale.timeToPx(visibleWindowTime.end));
    const zeroY = MARGIN_TOP + RECT_HEIGHT / (minimumValue < 0 ? 2 : 1);

    // Quantize the Y axis to quarters of powers of tens (7.5K, 10K, 12.5K).
    const maxValue = Math.max(maximumValue, 0);

    let yMax = Math.max(Math.abs(minimumValue), maxValue);
    const kUnits = ['', 'K', 'M', 'G', 'T', 'E'];
    const exp = Math.ceil(Math.log10(Math.max(yMax, 1)));
    const pow10 = Math.pow(10, exp);
    yMax = Math.ceil(yMax / (pow10 / 4)) * (pow10 / 4);
    let yRange = 0;
    const unitGroup = Math.floor(exp / 3);
    let yMin = 0;
    let yLabel = '';
    if (scale === 'MIN_MAX') {
      yRange = maximumValue - minimumValue;
      yMin = minimumValue;
      yLabel = 'min - max';
    } else {
      yRange = minimumValue < 0 ? yMax * 2 : yMax;
      yMin = minimumValue < 0 ? -yMax : 0;
      yLabel = `${yMax / Math.pow(10, unitGroup * 3)} ${kUnits[unitGroup]}`;
      if (scale === 'DELTA_FROM_PREVIOUS') {
        yLabel += '\u0394';
      } else if (scale === 'RATE') {
        yLabel += '\u0394/t';
      }
    }

    // There are 360deg of hue. We want a scale that starts at green with
    // exp <= 3 (<= 1KB), goes orange around exp = 6 (~1MB) and red/violet
    // around exp >= 9 (1GB).
    // The hue scale looks like this:
    // 0                              180                                 360
    // Red        orange         green | blue         purple          magenta
    // So we want to start @ 180deg with pow=0, go down to 0deg and then wrap
    // back from 360deg back to 180deg.
    const expCapped = Math.min(Math.max(exp - 3), 9);
    const hue = (180 - Math.floor(expCapped * (180 / 6)) + 360) % 360;

    ctx.fillStyle = `hsl(${hue}, 45%, 75%)`;
    ctx.strokeStyle = `hsl(${hue}, 45%, 45%)`;

    const calculateX = (ts: number) => {
      return Math.floor(timeScale.timeToPx(ts));
    };
    const calculateY = (value: number) => {
      return MARGIN_TOP + RECT_HEIGHT -
          Math.round(((value - yMin) / yRange) * RECT_HEIGHT);
    };

    ctx.beginPath();
    ctx.moveTo(calculateX(data.timestamps[0]), zeroY);
    let lastDrawnY = zeroY;
    for (let i = 0; i < data.timestamps.length; i++) {
      const x = calculateX(data.timestamps[i]);
      const minY = calculateY(minValues[i]);
      const maxY = calculateY(maxValues[i]);
      const lastY = calculateY(lastValues[i]);

      ctx.lineTo(x, lastDrawnY);
      if (minY === maxY) {
        assertTrue(lastY === minY);
        ctx.lineTo(x, lastY);
      } else {
        ctx.lineTo(x, minY);
        ctx.lineTo(x, maxY);
        ctx.lineTo(x, lastY);
      }
      lastDrawnY = lastY;
    }
    ctx.lineTo(endPx, lastDrawnY);
    ctx.lineTo(endPx, zeroY);
    ctx.closePath();
    ctx.fill();
    ctx.stroke();

    // Draw the Y=0 dashed line.
    ctx.strokeStyle = `hsl(${hue}, 10%, 71%)`;
    ctx.beginPath();
    ctx.setLineDash([2, 4]);
    ctx.moveTo(0, zeroY);
    ctx.lineTo(endPx, zeroY);
    ctx.closePath();
    ctx.stroke();
    ctx.setLineDash([]);

    ctx.font = '10px Roboto Condensed';

    if (this.hoveredValue !== undefined && this.hoveredTs !== undefined) {
      // TODO(hjd): Add units.
      let text: string;
      if (scale === 'DELTA_FROM_PREVIOUS') {
        text = 'delta: ';
      } else if (scale === 'RATE') {
        text = 'delta/t: ';
      } else {
        text = 'value: ';
      }

      text += `${this.hoveredValue.toLocaleString()}`;

      ctx.fillStyle = `hsl(${hue}, 45%, 75%)`;
      ctx.strokeStyle = `hsl(${hue}, 45%, 45%)`;

      const xStart = Math.floor(timeScale.timeToPx(this.hoveredTs));
      const xEnd = this.hoveredTsEnd === undefined ?
          endPx :
          Math.floor(timeScale.timeToPx(this.hoveredTsEnd));
      const y = MARGIN_TOP + RECT_HEIGHT -
          Math.round(((this.hoveredValue - yMin) / yRange) * RECT_HEIGHT);

      // Highlight line.
      ctx.beginPath();
      ctx.moveTo(xStart, y);
      ctx.lineTo(xEnd, y);
      ctx.lineWidth = 3;
      ctx.stroke();
      ctx.lineWidth = 1;

      // Draw change marker.
      ctx.beginPath();
      ctx.arc(
          xStart, y, 3 /* r*/, 0 /* start angle*/, 2 * Math.PI /* end angle*/);
      ctx.fill();
      ctx.stroke();

      // Draw the tooltip.
      this.drawTrackHoverTooltip(ctx, this.mousePos, text);
    }

    // Write the Y scale on the top left corner.
    ctx.fillStyle = 'rgba(255, 255, 255, 0.6)';
    ctx.fillRect(0, 0, 42, 16);
    ctx.fillStyle = '#666';
    ctx.textAlign = 'left';
    ctx.textBaseline = 'alphabetic';
    ctx.fillText(`${yLabel}`, 5, 14);

    // TODO(hjd): Refactor this into checkerboardExcept
    {
      const endPx = timeScale.timeToPx(visibleWindowTime.end);
      const counterEndPx =
          Math.min(timeScale.timeToPx(this.config.endTs || Infinity), endPx);

      // Grey out RHS.
      if (counterEndPx < endPx) {
        ctx.fillStyle = '#0000001f';
        ctx.fillRect(counterEndPx, 0, endPx - counterEndPx, this.getHeight());
      }
    }

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
        ctx,
        this.getHeight(),
        timeScale.timeToPx(visibleWindowTime.start),
        timeScale.timeToPx(visibleWindowTime.end),
        timeScale.timeToPx(data.start),
        timeScale.timeToPx(data.end));
  }

  onMouseMove(pos: {x: number, y: number}) {
    const data = this.data();
    if (data === undefined) return;
    this.mousePos = pos;
    const {timeScale} = globals.frontendLocalState;
    const time = timeScale.pxToTime(pos.x);

    const values = this.config.scale === 'DELTA_FROM_PREVIOUS' ?
        data.totalDeltas :
        data.lastValues;
    const [left, right] = searchSegment(data.timestamps, time);
    this.hoveredTs = left === -1 ? undefined : data.timestamps[left];
    this.hoveredTsEnd = right === -1 ? undefined : data.timestamps[right];
    this.hoveredValue = left === -1 ? undefined : values[left];
  }

  onMouseOut() {
    this.hoveredValue = undefined;
    this.hoveredTs = undefined;
  }

  onMouseClick({x}: {x: number}) {
    const data = this.data();
    if (data === undefined) return false;
    const {timeScale} = globals.frontendLocalState;
    const time = timeScale.pxToTime(x);
    const [left, right] = searchSegment(data.timestamps, time);
    if (left === -1) {
      return false;
    } else {
      const counterId = data.lastIds[left];
      if (counterId === -1) return true;
      globals.makeSelection(Actions.selectCounter({
        leftTs: toNs(data.timestamps[left]),
        rightTs: right !== -1 ? toNs(data.timestamps[right]) : -1,
        id: counterId,
        trackId: this.trackState.id,
      }));
      return true;
    }
  }
}

async function globalTrackProvider(engine: EngineProxy): Promise<TrackInfo[]> {
  const result = await engine.query(`
    select name, id
    from (
      select name, id
      from counter_track
      where type = 'counter_track'
      union
      select name, id
      from gpu_counter_track
      where name != 'gpufreq'
    )
    order by name
  `);

  // Add global or GPU counter tracks that are not bound to any pid/tid.
  const it = result.iter({
    name: STR,
    id: NUM,
  });

  const tracks: TrackInfo[] = [];
  for (; it.valid(); it.next()) {
    const name = it.name;
    const trackId = it.id;
    tracks.push({
      trackKind: COUNTER_TRACK_KIND,
      name,
      config: {
        name,
        trackId,
      },
    });
  }
  return tracks;
}

export function activate(ctx: PluginContext) {
  ctx.registerTrackController(CounterTrackController);
  ctx.registerTrack(CounterTrack);
  ctx.registerTrackProvider(globalTrackProvider);
}

export const plugin = {
  pluginId: 'perfetto.Counter',
  activate,
};
