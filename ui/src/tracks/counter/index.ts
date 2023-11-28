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

import m from 'mithril';
import {v4 as uuidv4} from 'uuid';

import {searchSegment} from '../../base/binary_search';
import {assertTrue} from '../../base/logging';
import {isString} from '../../base/object_utils';
import {duration, time, Time} from '../../base/time';
import {Actions} from '../../common/actions';
import {
  BasicAsyncTrack,
  NUM_NULL,
  STR_NULL,
} from '../../common/basic_async_track';
import {drawTrackHoverTooltip} from '../../common/canvas_utils';
import {TrackData} from '../../common/track_data';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {
  EngineProxy,
  LONG,
  LONG_NULL,
  NUM,
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
  PrimaryTrackSortKey,
  Store,
  STR,
  TrackContext,
} from '../../public';
import {getTrackName} from '../../public/utils';
import {Button} from '../../widgets/button';
import {MenuItem, PopupMenu2} from '../../widgets/menu';

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
  timestamps: BigInt64Array;
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
  startTs?: time;
  endTs?: time;
  namespace?: string;
  trackId: number;
  defaultScale?: CounterScaleOptions;
}

const NETWORK_TRACK_REGEX = new RegExp('^.* (Received|Transmitted)( KB)?$');
const ENTITY_RESIDENCY_REGEX = new RegExp('^Entity residency:');

// Sets the default 'scale' for counter tracks. If the regex matches
// then the paired mode is used. Entries are in priority order so the
// first match wins.
const COUNTER_REGEX: [RegExp, CounterScaleOptions][] = [
  // Power counters make more sense in rate mode since you're typically
  // interested in the slope of the graph rather than the absolute
  // value.
  [new RegExp('^power\..*$'), 'RATE'],
  // Same for network counters.
  [NETWORK_TRACK_REGEX, 'RATE'],
  // Entity residency
  [ENTITY_RESIDENCY_REGEX, 'RATE'],
];

function getCounterScale(name: string): CounterScaleOptions|undefined {
  for (const [re, scale] of COUNTER_REGEX) {
    if (name.match(re)) {
      return scale;
    }
  }
  return undefined;
}

// 0.5 Makes the horizontal lines sharp.
const MARGIN_TOP = 3.5;
const RECT_HEIGHT = 24.5;

interface CounterTrackState {
  scale: CounterScaleOptions;
}

function isCounterState(x: unknown): x is CounterTrackState {
  if (x && typeof x === 'object' && 'scale' in x) {
    if (isString(x.scale)) {
      return true;
    } else {
      return false;
    }
  } else {
    return false;
  }
}

export class CounterTrack extends BasicAsyncTrack<Data> {
  private maximumValueSeen = 0;
  private minimumValueSeen = 0;
  private maximumDeltaSeen = 0;
  private minimumDeltaSeen = 0;
  private maxDurNs: duration = 0n;
  private store: Store<CounterTrackState>;
  private trackKey: string;
  private uuid = uuidv4();
  private isSetup = false;

  constructor(
      ctx: TrackContext, private config: Config, private engine: EngineProxy) {
    super();
    this.trackKey = ctx.trackKey;
    this.store = ctx.mountStore<CounterTrackState>((init: unknown) => {
      if (isCounterState(init)) {
        return init;
      } else {
        return {scale: this.config.defaultScale ?? 'ZERO_BASED'};
      }
    });
  }

  // Returns a valid SQL table name with the given prefix that should be unique
  // for each track.
  tableName(prefix: string) {
    // Derive table name from, since that is unique for each track.
    // Track ID can be UUID but '-' is not valid for sql table name.
    const idSuffix = this.uuid.split('-').join('_');
    return `${prefix}_${idSuffix}`;
  }

  private namespaceTable(tableName: string): string {
    if (this.config.namespace) {
      return this.config.namespace + '_' + tableName;
    } else {
      return tableName;
    }
  }

  private async setup() {
    if (this.config.namespace === undefined) {
      await this.engine.query(`
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
      await this.engine.query(`
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

    const maxDurResult = await this.engine.query(`
        select
          max(
            iif(dur != -1, dur, (select end_ts from trace_bounds) - ts)
          ) as maxDur
        from ${this.tableName('counter_view')}
    `);
    this.maxDurNs = maxDurResult.firstRow({maxDur: LONG_NULL}).maxDur || 0n;

    const queryRes = await this.engine.query(`
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
  }

  async onBoundsChange(start: time, end: time, resolution: duration):
      Promise<Data> {
    if (!this.isSetup) {
      await this.setup();
      this.isSetup = true;
    }

    const queryRes = await this.engine.query(`
      select
        (ts + ${resolution / 2n}) / ${resolution} * ${resolution} as tsq,
        min(value) as minValue,
        max(value) as maxValue,
        sum(delta) as totalDelta,
        value_at_max_ts(ts, id) as lastId,
        value_at_max_ts(ts, value) as lastValue
      from ${this.tableName('counter_view')}
      where ts >= ${start - this.maxDurNs} and ts <= ${end}
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
      timestamps: new BigInt64Array(numRows),
      lastIds: new Float64Array(numRows),
      minValues: new Float64Array(numRows),
      maxValues: new Float64Array(numRows),
      lastValues: new Float64Array(numRows),
      totalDeltas: new Float64Array(numRows),
      rate: new Float64Array(numRows),
    };

    const it = queryRes.iter({
      'tsq': LONG,
      'lastId': NUM,
      'minValue': NUM,
      'maxValue': NUM,
      'lastValue': NUM,
      'totalDelta': NUM,
    });
    let lastValue = 0;
    let lastTs = 0n;
    for (let row = 0; it.valid(); it.next(), row++) {
      const ts = Time.fromRaw(it.tsq);
      const value = it.lastValue;
      const rate = (value - lastValue) / (Time.toSeconds(Time.sub(ts, lastTs)));
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

  private mousePos = {x: 0, y: 0};
  private hoveredValue: number|undefined = undefined;
  private hoveredTs: time|undefined = undefined;
  private hoveredTsEnd: time|undefined = undefined;

  getHeight() {
    return MARGIN_TOP + RECT_HEIGHT;
  }

  getTrackShellButtons(): m.Children {
    const currentScale = this.store.state.scale;
    const scales: {name: CounterScaleOptions, humanName: string}[] = [
      {name: 'ZERO_BASED', humanName: 'Zero based'},
      {name: 'MIN_MAX', humanName: 'Min/Max'},
      {name: 'DELTA_FROM_PREVIOUS', humanName: 'Delta'},
      {name: 'RATE', humanName: 'Rate'},
    ];
    const menuItems = scales.map((scale) => {
      return m(MenuItem, {
        label: scale.humanName,
        active: currentScale === scale.name,
        onclick: () => {
          this.store.edit((draft) => {
            draft.scale = scale.name;
          });
        },
      });
    });

    return m(
        PopupMenu2,
        {
          trigger: m(Button, {icon: 'show_chart', minimal: true}),
        },
        menuItems,
    );
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    // TODO: fonts and colors should come from the CSS and not hardcoded here.
    const {
      visibleTimeScale: timeScale,
      windowSpan,
    } = globals.frontendLocalState;
    const data = this.data;

    // Can't possibly draw anything.
    if (data === undefined || data.timestamps.length === 0) {
      return;
    }

    assertTrue(data.timestamps.length === data.minValues.length);
    assertTrue(data.timestamps.length === data.maxValues.length);
    assertTrue(data.timestamps.length === data.lastValues.length);
    assertTrue(data.timestamps.length === data.totalDeltas.length);
    assertTrue(data.timestamps.length === data.rate.length);

    const scale: CounterScaleOptions = this.store.state.scale;

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

    const endPx = windowSpan.end;
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

    const calculateX = (ts: time) => {
      return Math.floor(timeScale.timeToPx(ts));
    };
    const calculateY = (value: number) => {
      return MARGIN_TOP + RECT_HEIGHT -
          Math.round(((value - yMin) / yRange) * RECT_HEIGHT);
    };

    ctx.beginPath();
    const timestamp = Time.fromRaw(data.timestamps[0]);
    ctx.moveTo(calculateX(timestamp), zeroY);
    let lastDrawnY = zeroY;
    for (let i = 0; i < data.timestamps.length; i++) {
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
      let counterEndPx = Infinity;
      if (this.config.endTs) {
        counterEndPx = Math.min(timeScale.timeToPx(this.config.endTs), endPx);
      }

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
        timeScale.timeToPx(data.start),
        timeScale.timeToPx(data.end));
  }

  onMouseMove(pos: {x: number, y: number}) {
    const data = this.data;
    if (data === undefined) return;
    this.mousePos = pos;
    const {visibleTimeScale} = globals.frontendLocalState;
    const time = visibleTimeScale.pxToHpTime(pos.x);

    let values = data.lastValues;
    if (this.store.state.scale === 'DELTA_FROM_PREVIOUS') {
      values = data.totalDeltas;
    }
    if (this.store.state.scale === 'RATE') {
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

  onMouseClick({x}: {x: number}): boolean {
    const data = this.data;
    if (data === undefined) return false;
    const {visibleTimeScale} = globals.frontendLocalState;
    const time = visibleTimeScale.pxToHpTime(x);
    const [left, right] = searchSegment(data.timestamps, time.toTime());
    if (left === -1) {
      return false;
    } else {
      const counterId = data.lastIds[left];
      if (counterId === -1) return true;
      globals.makeSelection(Actions.selectCounter({
        leftTs: Time.fromRaw(data.timestamps[left]),
        rightTs: Time.fromRaw(right !== -1 ? data.timestamps[right] : -1n),
        id: counterId,
        trackKey: this.trackKey,
      }));
      return true;
    }
  }

  async onDestroy(): Promise<void> {
    await this.engine.query(
        `DROP VIEW IF EXISTS ${this.tableName('counter_view')}`);
  }
}

interface CounterInfo {
  name: string;
  trackId: number;
}

class CounterPlugin implements Plugin {
  onActivate(_ctx: PluginContext): void {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    await this.addCounterTracks(ctx);
    await this.addGpuFrequencyTracks(ctx);
    await this.addCpuFreqLimitCounterTracks(ctx);
    await this.addCpuPerfCounterTracks(ctx);
    await this.addThreadCounterTracks(ctx);
    await this.addProcessCounterTracks(ctx);
  }

  private async addCounterTracks(ctx: PluginContextTrace) {
    const counters = await this.getCounterNames(ctx.engine);
    for (const {trackId, name} of counters) {
      const config:
          Config = {name, trackId, defaultScale: getCounterScale(name)};
      const uri = `perfetto.Counter#${trackId}`;
      ctx.registerStaticTrack({
        uri,
        displayName: name,
        kind: COUNTER_TRACK_KIND,
        trackIds: [trackId],
        track: (trackCtx) => {
          return new CounterTrack(trackCtx, config, ctx.engine);
        },
      });
      ctx.addDefaultTrack({
        uri,
        displayName: name,
        sortKey: PrimaryTrackSortKey.COUNTER_TRACK,
      });
    }
  }

  private async getCounterNames(engine: EngineProxy): Promise<CounterInfo[]> {
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

    const tracks: CounterInfo[] = [];
    for (; it.valid(); it.next()) {
      tracks.push({
        trackId: it.id,
        name: it.name,
      });
    }
    return tracks;
  }

  private async addGpuFrequencyTracks(ctx: PluginContextTrace) {
    const engine = ctx.engine;
    const numGpus = await engine.getNumberOfGpus();
    const maxGpuFreqResult = await engine.query(`
      select ifnull(max(value), 0) as maximumValue
      from counter c
      inner join gpu_counter_track t on c.track_id = t.id
      where name = 'gpufreq';
    `);
    const maximumValue =
        maxGpuFreqResult.firstRow({maximumValue: NUM}).maximumValue;

    for (let gpu = 0; gpu < numGpus; gpu++) {
      // Only add a gpu freq track if we have
      // gpu freq data.
      const freqExistsResult = await engine.query(`
      select id
      from gpu_counter_track
      where name = 'gpufreq' and gpu_id = ${gpu}
      limit 1;
    `);
      if (freqExistsResult.numRows() > 0) {
        const trackId = freqExistsResult.firstRow({id: NUM}).id;
        const uri = `perfetto.Counter#gpu_freq${gpu}`;
        const name = `Gpu ${gpu} Frequency`;
        const config: Config = {
          name,
          trackId,
          maximumValue,
          defaultScale: getCounterScale(name),
        };
        ctx.registerStaticTrack({
          uri,
          displayName: name,
          kind: COUNTER_TRACK_KIND,
          trackIds: [trackId],
          track: (trackCtx) => {
            return new CounterTrack(trackCtx, config, ctx.engine);
          },
        });
      }
    }
  }

  async addCpuFreqLimitCounterTracks(ctx: PluginContextTrace): Promise<void> {
    const cpuFreqLimitCounterTracksSql = `
      select name, id
      from cpu_counter_track
      where name glob "Cpu * Freq Limit"
      order by name asc
    `;

    this.addCpuCounterTracks(ctx, cpuFreqLimitCounterTracksSql);
  }

  async addCpuPerfCounterTracks(ctx: PluginContextTrace): Promise<void> {
    // Perf counter tracks are bound to CPUs, follow the scheduling and
    // frequency track naming convention ("Cpu N ...").
    // Note: we might not have a track for a given cpu if no data was seen from
    // it. This might look surprising in the UI, but placeholder tracks are
    // wasteful as there's no way of collapsing global counter tracks at the
    // moment.
    const addCpuPerfCounterTracksSql = `
      select printf("Cpu %u %s", cpu, name) as name, id
      from perf_counter_track as pct
      order by perf_session_id asc, pct.name asc, cpu asc
    `;
    this.addCpuCounterTracks(ctx, addCpuPerfCounterTracksSql);
  }

  async addCpuCounterTracks(ctx: PluginContextTrace, sql: string):
      Promise<void> {
    const result = await ctx.engine.query(sql);

    const it = result.iter({
      name: STR,
      id: NUM,
    });

    for (; it.valid(); it.next()) {
      const name = it.name;
      const trackId = it.id;
      const config: Config = {
        name,
        trackId,
        defaultScale: getCounterScale(name),
      };
      ctx.registerStaticTrack({
        uri: `perfetto.Counter#cpu${trackId}`,
        displayName: name,
        kind: COUNTER_TRACK_KIND,
        trackIds: [trackId],
        track: (trackCtx) => {
          return new CounterTrack(trackCtx, config, ctx.engine);
        },
      });
    }
  }

  async addThreadCounterTracks(ctx: PluginContextTrace): Promise<void> {
    const result = await ctx.engine.query(`
      select
        thread_counter_track.name as trackName,
        utid,
        upid,
        tid,
        thread.name as threadName,
        thread_counter_track.id as trackId,
        thread.start_ts as startTs,
        thread.end_ts as endTs
      from thread_counter_track
      join thread using(utid)
      left join process using(upid)
      where thread_counter_track.name != 'thread_time'
    `);

    const it = result.iter({
      startTs: LONG_NULL,
      trackId: NUM,
      endTs: LONG_NULL,
      trackName: STR_NULL,
      utid: NUM,
      upid: NUM_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const tid = it.tid;
      const startTs = it.startTs === null ? undefined : it.startTs;
      const endTs = it.endTs === null ? undefined : it.endTs;
      const trackId = it.trackId;
      const trackName = it.trackName;
      const threadName = it.threadName;
      const kind = COUNTER_TRACK_KIND;
      const name = getTrackName({
        name: trackName,
        utid,
        tid,
        kind,
        threadName,
        threadTrack: true,
      });
      const config: Config = {
        name,
        trackId,
        startTs: Time.fromRaw(startTs),
        endTs: Time.fromRaw(endTs),
        defaultScale: getCounterScale(name),
      };
      ctx.registerStaticTrack({
        uri: `perfetto.Counter#thread${trackId}`,
        displayName: name,
        kind,
        trackIds: [trackId],
        track: (trackCtx) => {
          return new CounterTrack(trackCtx, config, ctx.engine);
        },
      });
    }
  }

  async addProcessCounterTracks(ctx: PluginContextTrace): Promise<void> {
    const result = await ctx.engine.query(`
    select
      process_counter_track.id as trackId,
      process_counter_track.name as trackName,
      upid,
      process.pid,
      process.name as processName,
      process.start_ts as startTs,
      process.end_ts as endTs
    from process_counter_track
    join process using(upid);
  `);
    const it = result.iter({
      trackId: NUM,
      trackName: STR_NULL,
      upid: NUM,
      startTs: LONG_NULL,
      endTs: LONG_NULL,
      pid: NUM_NULL,
      processName: STR_NULL,
    });
    for (let i = 0; it.valid(); ++i, it.next()) {
      const trackId = it.trackId;
      const startTs = it.startTs === null ? undefined : it.startTs;
      const endTs = it.endTs === null ? undefined : it.endTs;
      const pid = it.pid;
      const trackName = it.trackName;
      const upid = it.upid;
      const processName = it.processName;
      const kind = COUNTER_TRACK_KIND;
      const name = getTrackName({
        name: trackName,
        upid,
        pid,
        kind,
        processName,
      });
      const config: Config = {
        name,
        trackId,
        startTs: Time.fromRaw(startTs),
        endTs: Time.fromRaw(endTs),
        defaultScale: getCounterScale(name),
      };
      ctx.registerStaticTrack({
        uri: `perfetto.Counter#process${trackId}`,
        displayName: name,
        kind: COUNTER_TRACK_KIND,
        trackIds: [trackId],
        track: (trackCtx) => {
          return new CounterTrack(trackCtx, config, ctx.engine);
        },
      });
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.Counter',
  plugin: CounterPlugin,
};
