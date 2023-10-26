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
import {searchSegment} from '../../base/binary_search';
import {assertTrue} from '../../base/logging';
import {duration, time, Time} from '../../base/time';
import {calcCachedBucketSize} from '../../common/cache_utils';
import {drawTrackHoverTooltip} from '../../common/canvas_utils';
import {hueForCpu} from '../../common/colorizer';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  QueryResult,
} from '../../common/query_result';
import {
  TrackAdapter,
  TrackControllerAdapter,
  TrackWithControllerAdapter,
} from '../../common/track_adapter';
import {TrackData} from '../../common/track_data';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {NewTrackArgs} from '../../frontend/track';
import {
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';


export const CPU_FREQ_TRACK_KIND = 'CpuFreqTrack';

export interface Data extends TrackData {
  maximumValue: number;
  maxTsEnd: time;

  timestamps: BigInt64Array;
  minFreqKHz: Uint32Array;
  maxFreqKHz: Uint32Array;
  lastFreqKHz: Uint32Array;
  lastIdleValues: Int8Array;
}

export interface Config {
  cpu: number;
  freqTrackId: number;
  idleTrackId?: number;
  maximumValue?: number;
  minimumValue?: number;
}

class CpuFreqTrackController extends TrackControllerAdapter<Config, Data> {
  private maxDur: duration = 0n;
  private maxTsEnd: time = Time.ZERO;
  private maximumValueSeen = 0;
  private cachedBucketSize = BIMath.INT64_MAX;

  async onSetup() {
    await this.createFreqIdleViews();

    this.maximumValueSeen = await this.queryMaxFrequency();
    this.maxDur = await this.queryMaxSourceDur();

    const iter = (await this.query(`
      select max(ts) as maxTs, dur, count(1) as rowCount
      from ${this.tableName('freq_idle')}
    `)).firstRow({maxTs: LONG_NULL, dur: LONG_NULL, rowCount: NUM});
    if (iter.maxTs === null || iter.dur === null) {
      // We shoulnd't really hit this because trackDecider shouldn't create
      // the track in the first place if there are no entries. But could happen
      // if only one cpu has no cpufreq data.
      return;
    }
    this.maxTsEnd = Time.add(Time.fromRaw(iter.maxTs), iter.dur);

    const rowCount = iter.rowCount;
    const bucketSize = calcCachedBucketSize(rowCount);
    if (bucketSize === undefined) {
      return;
    }

    await this.query(`
      create table ${this.tableName('freq_idle_cached')} as
      select
        (ts + ${bucketSize / 2n}) / ${bucketSize} * ${bucketSize} as cachedTsq,
        min(freqValue) as minFreq,
        max(freqValue) as maxFreq,
        value_at_max_ts(ts, freqValue) as lastFreq,
        value_at_max_ts(ts, idleValue) as lastIdleValue
      from ${this.tableName('freq_idle')}
      group by cachedTsq
      order by cachedTsq
    `);

    this.cachedBucketSize = bucketSize;
  }

  async onBoundsChange(start: time, end: time, resolution: duration):
      Promise<Data> {
    // The resolution should always be a power of two for the logic of this
    // function to make sense.
    assertTrue(BIMath.popcount(resolution) === 1, `${resolution} not pow of 2`);

    const freqResult = await this.queryData(start, end, resolution);
    assertTrue(freqResult.isComplete());

    const numRows = freqResult.numRows();
    const data: Data = {
      start,
      end,
      resolution,
      length: numRows,
      maximumValue: this.maximumValue(),
      maxTsEnd: this.maxTsEnd,
      timestamps: new BigInt64Array(numRows),
      minFreqKHz: new Uint32Array(numRows),
      maxFreqKHz: new Uint32Array(numRows),
      lastFreqKHz: new Uint32Array(numRows),
      lastIdleValues: new Int8Array(numRows),
    };

    const it = freqResult.iter({
      'tsq': LONG,
      'minFreq': NUM,
      'maxFreq': NUM,
      'lastFreq': NUM,
      'lastIdleValue': NUM,
    });
    for (let i = 0; it.valid(); ++i, it.next()) {
      data.timestamps[i] = it.tsq;
      data.minFreqKHz[i] = it.minFreq;
      data.maxFreqKHz[i] = it.maxFreq;
      data.lastFreqKHz[i] = it.lastFreq;
      data.lastIdleValues[i] = it.lastIdleValue;
    }

    return data;
  }

  private async queryData(start: time, end: time, bucketSize: duration):
      Promise<QueryResult> {
    const isCached = this.cachedBucketSize <= bucketSize;

    if (isCached) {
      return this.query(`
        select
          cachedTsq / ${bucketSize} * ${bucketSize} as tsq,
          min(minFreq) as minFreq,
          max(maxFreq) as maxFreq,
          value_at_max_ts(cachedTsq, lastFreq) as lastFreq,
          value_at_max_ts(cachedTsq, lastIdleValue) as lastIdleValue
        from ${this.tableName('freq_idle_cached')}
        where
          cachedTsq >= ${start - this.maxDur} and
          cachedTsq <= ${end}
        group by tsq
        order by tsq
      `);
    }
    const minTsFreq = await this.query(`
      select ifnull(max(ts), 0) as minTs from ${this.tableName('freq')}
      where ts < ${start}
    `);

    let minTs = minTsFreq.iter({minTs: NUM}).minTs;
    if (this.config.idleTrackId !== undefined) {
      const minTsIdle = await this.query(`
        select ifnull(max(ts), 0) as minTs from ${this.tableName('idle')}
        where ts < ${start}
      `);
      minTs = Math.min(minTsIdle.iter({minTs: NUM}).minTs, minTs);
    }

    const geqConstraint = this.config.idleTrackId === undefined ?
        `ts >= ${minTs}` :
        `source_geq(ts, ${minTs})`;
    return this.query(`
      select
        (ts + ${bucketSize / 2n}) / ${bucketSize} * ${bucketSize} as tsq,
        min(freqValue) as minFreq,
        max(freqValue) as maxFreq,
        value_at_max_ts(ts, freqValue) as lastFreq,
        value_at_max_ts(ts, idleValue) as lastIdleValue
      from ${this.tableName('freq_idle')}
      where
        ${geqConstraint} and
        ts <= ${end}
      group by tsq
      order by tsq
    `);
  }

  private async queryMaxFrequency(): Promise<number> {
    const result = await this.query(`
      select max(freqValue) as maxFreq
      from ${this.tableName('freq')}
    `);
    return result.firstRow({'maxFreq': NUM_NULL}).maxFreq || 0;
  }

  private async queryMaxSourceDur(): Promise<duration> {
    const maxDurFreqResult = await this.query(
        `select ifnull(max(dur), 0) as maxDur from ${this.tableName('freq')}`);
    const maxDur = maxDurFreqResult.firstRow({'maxDur': LONG}).maxDur;
    if (this.config.idleTrackId === undefined) {
      return maxDur;
    }

    const maxDurIdleResult = await this.query(
        `select ifnull(max(dur), 0) as maxDur from ${this.tableName('idle')}`);
    return BIMath.max(maxDur, maxDurIdleResult.firstRow({maxDur: LONG}).maxDur);
  }

  private async createFreqIdleViews() {
    await this.query(`create view ${this.tableName('freq')} as
      select
        ts,
        dur,
        value as freqValue
      from experimental_counter_dur c
      where track_id = ${this.config.freqTrackId};
    `);

    if (this.config.idleTrackId === undefined) {
      await this.query(`create view ${this.tableName('freq_idle')} as
        select
          ts,
          dur,
          -1 as idleValue,
          freqValue
        from ${this.tableName('freq')};
      `);
      return;
    }

    await this.query(`
      create view ${this.tableName('idle')} as
      select
        ts,
        dur,
        iif(value = 4294967295, -1, cast(value as int)) as idleValue
      from experimental_counter_dur c
      where track_id = ${this.config.idleTrackId};
    `);

    await this.query(`
      create virtual table ${this.tableName('freq_idle')}
      using span_join(${this.tableName('freq')}, ${this.tableName('idle')});
    `);
  }

  private maximumValue() {
    return Math.max(this.config.maximumValue || 0, this.maximumValueSeen);
  }
}

// 0.5 Makes the horizontal lines sharp.
const MARGIN_TOP = 4.5;
const RECT_HEIGHT = 20;

class CpuFreqTrack extends TrackAdapter<Config, Data> {
  static create(args: NewTrackArgs): CpuFreqTrack {
    return new CpuFreqTrack(args);
  }

  private mousePos = {x: 0, y: 0};
  private hoveredValue: number|undefined = undefined;
  private hoveredTs: time|undefined = undefined;
  private hoveredTsEnd: time|undefined = undefined;
  private hoveredIdle: number|undefined = undefined;

  constructor(args: NewTrackArgs) {
    super(args);
  }

  getHeight() {
    return MARGIN_TOP + RECT_HEIGHT;
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    // TODO: fonts and colors should come from the CSS and not hardcoded here.
    const {
      visibleTimeScale,
      visibleWindowTime,
      windowSpan,
    } = globals.frontendLocalState;
    const data = this.data();

    if (data === undefined || data.timestamps.length === 0) {
      // Can't possibly draw anything.
      return;
    }

    assertTrue(data.timestamps.length === data.lastFreqKHz.length);
    assertTrue(data.timestamps.length === data.minFreqKHz.length);
    assertTrue(data.timestamps.length === data.maxFreqKHz.length);
    assertTrue(data.timestamps.length === data.lastIdleValues.length);

    const endPx = windowSpan.end;
    const zeroY = MARGIN_TOP + RECT_HEIGHT;

    // Quantize the Y axis to quarters of powers of tens (7.5K, 10K, 12.5K).
    let yMax = data.maximumValue;
    const kUnits = ['', 'K', 'M', 'G', 'T', 'E'];
    const exp = Math.ceil(Math.log10(Math.max(yMax, 1)));
    const pow10 = Math.pow(10, exp);
    yMax = Math.ceil(yMax / (pow10 / 4)) * (pow10 / 4);
    const unitGroup = Math.floor(exp / 3);
    const num = yMax / Math.pow(10, unitGroup * 3);
    // The values we have for cpufreq are in kHz so +1 to unitGroup.
    const yLabel = `${num} ${kUnits[unitGroup + 1]}Hz`;

    // Draw the CPU frequency graph.
    const hue = hueForCpu(this.config.cpu);
    let saturation = 45;
    if (globals.state.hoveredUtid !== -1) {
      saturation = 0;
    }
    ctx.fillStyle = `hsl(${hue}, ${saturation}%, 70%)`;
    ctx.strokeStyle = `hsl(${hue}, ${saturation}%, 55%)`;

    const calculateX = (timestamp: time) => {
      return Math.floor(visibleTimeScale.timeToPx(timestamp));
    };
    const calculateY = (value: number) => {
      return zeroY - Math.round((value / yMax) * RECT_HEIGHT);
    };

    const start = visibleWindowTime.start;
    const end = visibleWindowTime.end;
    const [rawStartIdx] = searchSegment(data.timestamps, start.toTime());
    const startIdx = rawStartIdx === -1 ? 0 : rawStartIdx;

    const [, rawEndIdx] = searchSegment(data.timestamps, end.toTime());
    const endIdx = rawEndIdx === -1 ? data.timestamps.length : rawEndIdx;

    ctx.beginPath();
    const timestamp = Time.fromRaw(data.timestamps[startIdx]);
    ctx.moveTo(Math.max(calculateX(timestamp), 0), zeroY);

    let lastDrawnY = zeroY;
    for (let i = startIdx; i < endIdx; i++) {
      const timestamp = Time.fromRaw(data.timestamps[i]);
      const x = calculateX(timestamp);

      const minY = calculateY(data.minFreqKHz[i]);
      const maxY = calculateY(data.maxFreqKHz[i]);
      const lastY = calculateY(data.lastFreqKHz[i]);

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
    // Find the end time for the last frequency event and then draw
    // down to zero to show that we do not have data after that point.
    const finalX = Math.min(calculateX(data.maxTsEnd), endPx);
    ctx.lineTo(finalX, lastDrawnY);
    ctx.lineTo(finalX, zeroY);
    ctx.lineTo(endPx, zeroY);
    ctx.closePath();
    ctx.fill();
    ctx.stroke();

    // Draw CPU idle rectangles that overlay the CPU freq graph.
    ctx.fillStyle = `rgba(240, 240, 240, 1)`;

    for (let i = 0; i < data.lastIdleValues.length; i++) {
      if (data.lastIdleValues[i] < 0) {
        continue;
      }

      // We intentionally don't use the floor function here when computing x
      // coordinates. Instead we use floating point which prevents flickering as
      // we pan and zoom; this relies on the browser anti-aliasing pixels
      // correctly.
      const timestamp = Time.fromRaw(data.timestamps[i]);
      const x = visibleTimeScale.timeToPx(timestamp);
      const xEnd = i === data.lastIdleValues.length - 1 ?
          finalX :
          visibleTimeScale.timeToPx(Time.fromRaw(data.timestamps[i + 1]));

      const width = xEnd - x;
      const height = calculateY(data.lastFreqKHz[i]) - zeroY;

      ctx.fillRect(x, zeroY, width, height);
    }

    ctx.font = '10px Roboto Condensed';

    if (this.hoveredValue !== undefined && this.hoveredTs !== undefined) {
      let text = `${this.hoveredValue.toLocaleString()}kHz`;

      ctx.fillStyle = `hsl(${hue}, 45%, 75%)`;
      ctx.strokeStyle = `hsl(${hue}, 45%, 45%)`;

      const xStart = Math.floor(visibleTimeScale.timeToPx(this.hoveredTs));
      const xEnd = this.hoveredTsEnd === undefined ?
          endPx :
          Math.floor(visibleTimeScale.timeToPx(this.hoveredTsEnd));
      const y = zeroY - Math.round((this.hoveredValue / yMax) * RECT_HEIGHT);

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

      // Display idle value if current hover is idle.
      if (this.hoveredIdle !== undefined && this.hoveredIdle !== -1) {
        // Display the idle value +1 to be consistent with catapult.
        text += ` (Idle: ${(this.hoveredIdle + 1).toLocaleString()})`;
      }

      // Draw the tooltip.
      drawTrackHoverTooltip(ctx, this.mousePos, this.getHeight(), text);
    }

    // Write the Y scale on the top left corner.
    ctx.textBaseline = 'alphabetic';
    ctx.fillStyle = 'rgba(255, 255, 255, 0.6)';
    ctx.fillRect(0, 0, 42, 18);
    ctx.fillStyle = '#666';
    ctx.textAlign = 'left';
    ctx.fillText(`${yLabel}`, 4, 14);

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
        ctx,
        this.getHeight(),
        windowSpan.start,
        windowSpan.end,
        visibleTimeScale.timeToPx(data.start),
        visibleTimeScale.timeToPx(data.end));
  }

  onMouseMove(pos: {x: number, y: number}) {
    const data = this.data();
    if (data === undefined) return;
    this.mousePos = pos;
    const {visibleTimeScale} = globals.frontendLocalState;
    const time = visibleTimeScale.pxToHpTime(pos.x);

    const [left, right] = searchSegment(data.timestamps, time.toTime());
    this.hoveredTs =
        left === -1 ? undefined : Time.fromRaw(data.timestamps[left]);
    this.hoveredTsEnd =
        right === -1 ? undefined : Time.fromRaw(data.timestamps[right]);
    this.hoveredValue = left === -1 ? undefined : data.lastFreqKHz[left];
    this.hoveredIdle = left === -1 ? undefined : data.lastIdleValues[left];
  }

  onMouseOut() {
    this.hoveredValue = undefined;
    this.hoveredTs = undefined;
    this.hoveredTsEnd = undefined;
    this.hoveredIdle = undefined;
  }
}

class CpuFreq implements Plugin {
  onActivate(_ctx: PluginContext): void {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const {engine} = ctx;

    const cpus = await engine.getCpus();

    const maxCpuFreqResult = await engine.query(`
      select ifnull(max(value), 0) as freq
      from counter c
      inner join cpu_counter_track t on c.track_id = t.id
      where name = 'cpufreq';
    `);
    const maxCpuFreq = maxCpuFreqResult.firstRow({freq: NUM}).freq;

    for (const cpu of cpus) {
      // Only add a cpu freq track if we have
      // cpu freq data.
      // TODO(hjd): Find a way to display cpu idle
      // events even if there are no cpu freq events.
      const cpuFreqIdleResult = await engine.query(`
        select
          id as cpuFreqId,
          (
            select id
            from cpu_counter_track
            where name = 'cpuidle'
            and cpu = ${cpu}
            limit 1
          ) as cpuIdleId
        from cpu_counter_track
        where name = 'cpufreq' and cpu = ${cpu}
        limit 1;
      `);

      if (cpuFreqIdleResult.numRows() > 0) {
        const row = cpuFreqIdleResult.firstRow({
          cpuFreqId: NUM,
          cpuIdleId: NUM_NULL,
        });
        const freqTrackId = row.cpuFreqId;
        const idleTrackId = row.cpuIdleId === null ? undefined : row.cpuIdleId;

        ctx.registerStaticTrack({
          uri: `perfetto.CpuFreq#${cpu}`,
          displayName: `Cpu ${cpu} Frequency`,
          kind: CPU_FREQ_TRACK_KIND,
          cpu,
          track: ({trackKey}) => {
            return new TrackWithControllerAdapter<Config, Data>(
                engine,
                trackKey,
                {
                  cpu,
                  maximumValue: maxCpuFreq,
                  freqTrackId,
                  idleTrackId,
                },
                CpuFreqTrack,
                CpuFreqTrackController);
          },
        });
      }
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.CpuFreq',
  plugin: CpuFreq,
};
