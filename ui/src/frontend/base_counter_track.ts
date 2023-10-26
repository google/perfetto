// Copyright (C) 2023 The Android Open Source Project
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

import {searchSegment} from '../base/binary_search';
import {assertTrue} from '../base/logging';
import {duration, Time, time} from '../base/time';
import {drawTrackHoverTooltip} from '../common/canvas_utils';
import {raf} from '../core/raf_scheduler';
import {LONG, NUM} from '../public';
import {CounterScaleOptions} from '../tracks/counter';
import {Button} from '../widgets/button';
import {MenuItem, PopupMenu2} from '../widgets/menu';

import {checkerboardExcept} from './checkerboard';
import {globals} from './globals';
import {constraintsToQuerySuffix} from './sql_utils';
import {NewTrackArgs, TrackBase} from './track';
import {CacheKey, TrackCache} from './track_cache';

interface CounterData {
  timestamps: BigInt64Array;
  minValues: Float64Array;
  maxValues: Float64Array;
  lastValues: Float64Array;
  totalDeltas: Float64Array;
  rate: Float64Array;
  maximumValue: number;
  minimumValue: number;
  maximumDelta: number;
  minimumDelta: number;
  maximumRate: number;
  minimumRate: number;
}

// 0.5 Makes the horizontal lines sharp.
const MARGIN_TOP = 3.5;

export abstract class BaseCounterTrack<Config> extends TrackBase<Config> {
  protected readonly tableName: string;

  // This is the over-skirted cached bounds:
  private countersKey: CacheKey = CacheKey.zero();

  private counters: CounterData = {
    timestamps: new BigInt64Array(0),
    minValues: new Float64Array(0),
    maxValues: new Float64Array(0),
    lastValues: new Float64Array(0),
    totalDeltas: new Float64Array(0),
    rate: new Float64Array(0),
    maximumValue: 0,
    minimumValue: 0,
    maximumDelta: 0,
    minimumDelta: 0,
    maximumRate: 0,
    minimumRate: 0,
  };

  private cache: TrackCache<CounterData> = new TrackCache(5);

  private sqlState: 'UNINITIALIZED'|'INITIALIZING'|'QUERY_PENDING'|
      'QUERY_DONE' = 'UNINITIALIZED';
  private isDestroyed: boolean = false;

  private maximumValueSeen = 0;
  private minimumValueSeen = 0;
  private maximumDeltaSeen = 0;
  private minimumDeltaSeen = 0;
  private maxDurNs: duration = 0n;

  private mousePos = {x: 0, y: 0};
  private hoveredValue: number|undefined = undefined;
  private hoveredTs: time|undefined = undefined;
  private hoveredTsEnd: time|undefined = undefined;

  private scale?: CounterScaleOptions;

  // Extension points.
  abstract initSqlTable(_tableName: string): Promise<void>;

  constructor(args: NewTrackArgs) {
    super(args);
    this.tableName = `track_${this.trackKey}`.replace(/[^a-zA-Z0-9_]+/g, '_');
  }

  getHeight() {
    return 30;
  }

  getCounterContextMenuItems(): m.Children {
    const currentScale = this.scale;
    const scales: {name: CounterScaleOptions, humanName: string}[] = [
      {name: 'ZERO_BASED', humanName: 'Zero based'},
      {name: 'MIN_MAX', humanName: 'Min/Max'},
      {name: 'DELTA_FROM_PREVIOUS', humanName: 'Delta'},
      {name: 'RATE', humanName: 'Rate'},
    ];
    return scales.map((scale) => {
      return m(MenuItem, {
        label: scale.humanName,
        active: currentScale === scale.name,
        onclick: () => {
          this.scale = scale.name;
          raf.scheduleFullRedraw();
        },
      });
    });
  }

  getCounterContextMenu(): m.Child {
    return m(
        PopupMenu2,
        {
          trigger: m(Button, {icon: 'show_chart', minimal: true}),
        },
        this.getCounterContextMenuItems(),
    );
  }

  getTrackShellButtons(): m.Children {
    return this.getCounterContextMenu();
  }

  renderCanvas(ctx: CanvasRenderingContext2D) {
    const {
      visibleTimeScale: timeScale,
      visibleWindowTime: vizTime,
      windowSpan,
    } = globals.frontendLocalState;

    {
      const windowSizePx = Math.max(1, timeScale.pxSpan.delta);
      const rawStartNs = vizTime.start.toTime();
      const rawEndNs = vizTime.end.toTime();
      const rawCountersKey =
          CacheKey.create(rawStartNs, rawEndNs, windowSizePx);

      // If the visible time range is outside the cached area, requests
      // asynchronously new data from the SQL engine.
      this.maybeRequestData(rawCountersKey);
    }

    // In any case, draw whatever we have (which might be stale/incomplete).

    if (this.counters === undefined || this.counters.timestamps.length === 0) {
      return;
    }

    const data = this.counters;
    assertTrue(data.timestamps.length === data.minValues.length);
    assertTrue(data.timestamps.length === data.maxValues.length);
    assertTrue(data.timestamps.length === data.lastValues.length);
    assertTrue(data.timestamps.length === data.totalDeltas.length);
    assertTrue(data.timestamps.length === data.rate.length);

    const scale: CounterScaleOptions = this.scale ?? 'ZERO_BASED';

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

    const effectiveHeight = this.getHeight() - MARGIN_TOP;
    const endPx = windowSpan.end;
    const zeroY = MARGIN_TOP + effectiveHeight / (minimumValue < 0 ? 2 : 1);

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

    const calculateX = (ts: time) => {
      return Math.floor(timeScale.timeToPx(ts));
    };
    const calculateY = (value: number) => {
      return MARGIN_TOP + effectiveHeight -
          Math.round(((value - yMin) / yRange) * effectiveHeight);
    };

    ctx.beginPath();
    const timestamp = Time.fromRaw(data.timestamps[0]);
    ctx.moveTo(calculateX(timestamp), zeroY);
    let lastDrawnY = zeroY;
    for (let i = 0; i < this.counters.timestamps.length; i++) {
      const timestamp = Time.fromRaw(data.timestamps[i]);
      const x = calculateX(timestamp);
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
      const y = MARGIN_TOP + effectiveHeight -
          Math.round(((this.hoveredValue - yMin) / yRange) * effectiveHeight);

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
      drawTrackHoverTooltip(ctx, this.mousePos, this.getHeight(), text);
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
      const counterEndPx = Infinity;
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
        windowSpan.start,
        windowSpan.end,
        timeScale.timeToPx(this.countersKey.start),
        timeScale.timeToPx(this.countersKey.end));
  }

  onMouseMove(pos: {x: number, y: number}) {
    const data = this.counters;
    if (data === undefined) return;
    this.mousePos = pos;
    const {visibleTimeScale} = globals.frontendLocalState;
    const time = visibleTimeScale.pxToHpTime(pos.x);

    let values = data.lastValues;
    if (this.scale === 'DELTA_FROM_PREVIOUS') {
      values = data.totalDeltas;
    }
    if (this.scale === 'RATE') {
      values = data.rate;
    }

    const [left, right] = searchSegment(data.timestamps, time.toTime());
    this.hoveredTs =
        left === -1 ? undefined : Time.fromRaw(data.timestamps[left]);
    this.hoveredTsEnd =
        right === -1 ? undefined : Time.fromRaw(data.timestamps[right]);
    this.hoveredValue = left === -1 ? undefined : values[left];
  }

  onMouseOut() {
    this.hoveredValue = undefined;
    this.hoveredTs = undefined;
  }

  // The underlying table has `ts` and `value` columns, but we also want to
  // query `dur` and `delta` - we create a CTE to help with that.
  private getSqlPreamble(): string {
    return `
      WITH data AS (
        SELECT
          ts,
          value,
          lead(ts, 1, ts) over (order by ts) - ts as dur,
          lead(value, 1, value) over (order by ts) - value as delta
        FROM ${this.tableName}
      )
    `;
  }

  private async maybeRequestData(rawCountersKey: CacheKey) {
    // Important: this method is async and is invoked on every frame. Care
    // must be taken to avoid piling up queries on every frame, hence the FSM.
    // TODO(altimin): Currently this is a copy of the logic in base_slice_track.
    // Consider merging it.
    if (this.sqlState === 'UNINITIALIZED') {
      this.sqlState = 'INITIALIZING';

      if (this.isDestroyed) {
        return;
      }
      await this.initSqlTable(this.tableName);

      if (this.isDestroyed) {
        return;
      }

      {
        const queryRes = (await this.engine.query(`
          ${this.getSqlPreamble()}
          SELECT
            ifnull(max(value), 0) as maxValue,
            ifnull(min(value), 0) as minValue,
            ifnull(max(delta), 0) as maxDelta,
            ifnull(min(delta), 0) as minDelta,
            max(
              iif(dur != -1, dur, (select end_ts from trace_bounds) - ts)
            ) as maxDur
          FROM data
        `)).firstRow({
          maxValue: NUM,
          minValue: NUM,
          maxDelta: NUM,
          minDelta: NUM,
          maxDur: LONG,
        });

        this.minimumValueSeen = queryRes.minValue;
        this.maximumValueSeen = queryRes.maxValue;
        this.minimumDeltaSeen = queryRes.minDelta;
        this.maximumDeltaSeen = queryRes.maxDelta;
        this.maxDurNs = queryRes.maxDur;
      }

      this.sqlState = 'QUERY_DONE';
    } else if (
        this.sqlState === 'INITIALIZING' || this.sqlState === 'QUERY_PENDING') {
      return;
    }

    if (rawCountersKey.isCoveredBy(this.countersKey)) {
      return;  // We have the data already, no need to re-query.
    }

    const countersKey = rawCountersKey.normalize();
    if (!rawCountersKey.isCoveredBy(countersKey)) {
      throw new Error(`Normalization error ${countersKey.toString()} ${
          rawCountersKey.toString()}`);
    }

    const maybeCachedCounters = this.cache.lookup(countersKey);
    if (maybeCachedCounters) {
      this.countersKey = countersKey;
      this.counters = maybeCachedCounters;
    }

    this.sqlState = 'QUERY_PENDING';
    const bucketNs = countersKey.bucketSize;

    const constraint = constraintsToQuerySuffix({
      filters: [
        `ts >= ${countersKey.start} - ${this.maxDurNs}`,
        `ts <= ${countersKey.end}`,
      ],
      groupBy: [
        'tsq',
      ],
      orderBy: [
        'tsq',
      ],
    });

    if (this.isDestroyed) {
      this.sqlState = 'QUERY_DONE';
      return;
    }

    const queryRes = await this.engine.query(`
      ${this.getSqlPreamble()}
      SELECT
        (ts + ${bucketNs / 2n}) / ${bucketNs} * ${bucketNs} as tsq,
        min(value) as minValue,
        max(value) as maxValue,
        sum(delta) as totalDelta,
        value_at_max_ts(ts, value) as lastValue
      FROM data
      ${constraint}
    `);

    const it = queryRes.iter({
      tsq: LONG,
      minValue: NUM,
      maxValue: NUM,
      totalDelta: NUM,
      lastValue: NUM,
    });

    const numRows = queryRes.numRows();
    const data: CounterData = {
      maximumValue: this.maximumValueSeen,
      minimumValue: this.minimumValueSeen,
      maximumDelta: this.maximumDeltaSeen,
      minimumDelta: this.minimumDeltaSeen,
      maximumRate: 0,
      minimumRate: 0,
      timestamps: new BigInt64Array(numRows),
      minValues: new Float64Array(numRows),
      maxValues: new Float64Array(numRows),
      lastValues: new Float64Array(numRows),
      totalDeltas: new Float64Array(numRows),
      rate: new Float64Array(numRows),
    };

    let lastValue = 0;
    let lastTs = 0n;
    for (let row = 0; it.valid(); it.next(), row++) {
      const ts = Time.fromRaw(it.tsq);
      const value = it.lastValue;
      const rate = (value - lastValue) / (Time.toSeconds(Time.sub(ts, lastTs)));
      lastTs = ts;
      lastValue = value;

      data.timestamps[row] = ts;
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

    this.cache.insert(countersKey, data);
    this.counters = data;

    this.sqlState = 'QUERY_DONE';
    raf.scheduleRedraw();
  }
}
