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
import {drawTrackHoverTooltip} from '../../common/canvas_utils';
import {colorForCpu} from '../../core/colorizer';
import {TrackData} from '../../common/track_data';
import {TimelineFetcher} from '../../common/track_helper';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {PanelSize} from '../../frontend/panel';
import {
  Engine,
  Plugin,
  PluginContextTrace,
  PluginDescriptor,
  Track,
} from '../../public';
import {LONG, NUM, NUM_NULL} from '../../trace_processor/query_result';
import {uuidv4Sql} from '../../base/uuid';

export const CPU_FREQ_TRACK_KIND = 'CpuFreqTrack';

export interface Data extends TrackData {
  timestamps: BigInt64Array;
  minFreqKHz: Uint32Array;
  maxFreqKHz: Uint32Array;
  lastFreqKHz: Uint32Array;
  lastIdleValues: Int8Array;
}

interface Config {
  cpu: number;
  freqTrackId: number;
  idleTrackId?: number;
  maximumValue: number;
}

// 0.5 Makes the horizontal lines sharp.
const MARGIN_TOP = 4.5;
const RECT_HEIGHT = 20;

class CpuFreqTrack implements Track {
  private mousePos = {x: 0, y: 0};
  private hoveredValue: number | undefined = undefined;
  private hoveredTs: time | undefined = undefined;
  private hoveredTsEnd: time | undefined = undefined;
  private hoveredIdle: number | undefined = undefined;
  private fetcher = new TimelineFetcher<Data>(this.onBoundsChange.bind(this));

  private engine: Engine;
  private config: Config;
  private trackUuid = uuidv4Sql();

  constructor(config: Config, engine: Engine) {
    this.config = config;
    this.engine = engine;
  }

  async onCreate() {
    if (this.config.idleTrackId === undefined) {
      await this.engine.query(`
        create view raw_freq_idle_${this.trackUuid} as
        select ts, dur, value as freqValue, -1 as idleValue
        from experimental_counter_dur c
        where track_id = ${this.config.freqTrackId}
      `);
    } else {
      await this.engine.query(`
        create view raw_freq_${this.trackUuid} as
        select ts, dur, value as freqValue
        from experimental_counter_dur c
        where track_id = ${this.config.freqTrackId};

        create view raw_idle_${this.trackUuid} as
        select
          ts,
          dur,
          iif(value = 4294967295, -1, cast(value as int)) as idleValue
        from experimental_counter_dur c
        where track_id = ${this.config.idleTrackId};

        create virtual table raw_freq_idle_${this.trackUuid}
        using span_join(raw_freq_${this.trackUuid}, raw_idle_${this.trackUuid});
      `);
    }

    await this.engine.query(`
      create virtual table cpu_freq_${this.trackUuid}
      using __intrinsic_counter_mipmap((
        select ts, freqValue as value
        from raw_freq_idle_${this.trackUuid}
      ));

      create virtual table cpu_idle_${this.trackUuid}
      using __intrinsic_counter_mipmap((
        select ts, idleValue as value
        from raw_freq_idle_${this.trackUuid}
      ));
    `);
  }

  async onUpdate() {
    await this.fetcher.requestDataForCurrentTime();
  }

  async onDestroy(): Promise<void> {
    await this.engine.tryQuery(`drop table cpu_freq_${this.trackUuid}`);
    await this.engine.tryQuery(`drop table cpu_idle_${this.trackUuid}`);
    await this.engine.tryQuery(`drop table raw_freq_idle_${this.trackUuid}`);
    await this.engine.tryQuery(
      `drop view if exists raw_freq_${this.trackUuid}`,
    );
    await this.engine.tryQuery(
      `drop view if exists raw_idle_${this.trackUuid}`,
    );
  }

  async onBoundsChange(
    start: time,
    end: time,
    resolution: duration,
  ): Promise<Data> {
    // The resolution should always be a power of two for the logic of this
    // function to make sense.
    assertTrue(BIMath.popcount(resolution) === 1, `${resolution} not pow of 2`);

    const freqResult = await this.engine.query(`
      SELECT
        min_value as minFreq,
        max_value as maxFreq,
        last_ts as ts,
        last_value as lastFreq
      FROM cpu_freq_${this.trackUuid}(
        ${start},
        ${end},
        ${resolution}
      );
    `);
    const idleResult = await this.engine.query(`
      SELECT last_value as lastIdle
      FROM cpu_idle_${this.trackUuid}(
        ${start},
        ${end},
        ${resolution}
      );
    `);

    const freqRows = freqResult.numRows();
    const idleRows = idleResult.numRows();
    assertTrue(freqRows == idleRows);

    const data: Data = {
      start,
      end,
      resolution,
      length: freqRows,
      timestamps: new BigInt64Array(freqRows),
      minFreqKHz: new Uint32Array(freqRows),
      maxFreqKHz: new Uint32Array(freqRows),
      lastFreqKHz: new Uint32Array(freqRows),
      lastIdleValues: new Int8Array(freqRows),
    };

    const freqIt = freqResult.iter({
      ts: LONG,
      minFreq: NUM,
      maxFreq: NUM,
      lastFreq: NUM,
    });
    const idleIt = idleResult.iter({
      lastIdle: NUM,
    });
    for (let i = 0; freqIt.valid(); ++i, freqIt.next(), idleIt.next()) {
      data.timestamps[i] = freqIt.ts;
      data.minFreqKHz[i] = freqIt.minFreq;
      data.maxFreqKHz[i] = freqIt.maxFreq;
      data.lastFreqKHz[i] = freqIt.lastFreq;
      data.lastIdleValues[i] = idleIt.lastIdle;
    }
    return data;
  }

  getHeight() {
    return MARGIN_TOP + RECT_HEIGHT;
  }

  render(ctx: CanvasRenderingContext2D, size: PanelSize): void {
    // TODO: fonts and colors should come from the CSS and not hardcoded here.
    const {visibleTimeScale, visibleWindowTime} = globals.timeline;
    const data = this.fetcher.data;

    if (data === undefined || data.timestamps.length === 0) {
      // Can't possibly draw anything.
      return;
    }

    assertTrue(data.timestamps.length === data.lastFreqKHz.length);
    assertTrue(data.timestamps.length === data.minFreqKHz.length);
    assertTrue(data.timestamps.length === data.maxFreqKHz.length);
    assertTrue(data.timestamps.length === data.lastIdleValues.length);

    const endPx = size.width;
    const zeroY = MARGIN_TOP + RECT_HEIGHT;

    // Quantize the Y axis to quarters of powers of tens (7.5K, 10K, 12.5K).
    let yMax = this.config.maximumValue;
    const kUnits = ['', 'K', 'M', 'G', 'T', 'E'];
    const exp = Math.ceil(Math.log10(Math.max(yMax, 1)));
    const pow10 = Math.pow(10, exp);
    yMax = Math.ceil(yMax / (pow10 / 4)) * (pow10 / 4);
    const unitGroup = Math.floor(exp / 3);
    const num = yMax / Math.pow(10, unitGroup * 3);
    // The values we have for cpufreq are in kHz so +1 to unitGroup.
    const yLabel = `${num} ${kUnits[unitGroup + 1]}Hz`;

    const color = colorForCpu(this.config.cpu);
    let saturation = 45;
    if (globals.state.hoveredUtid !== -1) {
      saturation = 0;
    }

    ctx.fillStyle = color.setHSL({s: saturation, l: 70}).cssString;
    ctx.strokeStyle = color.setHSL({s: saturation, l: 55}).cssString;

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

    // Draw the CPU frequency graph.
    {
      ctx.beginPath();
      const timestamp = Time.fromRaw(data.timestamps[startIdx]);
      ctx.moveTo(Math.max(calculateX(timestamp), 0), zeroY);

      let lastDrawnY = zeroY;
      for (let i = startIdx; i < endIdx; i++) {
        const timestamp = Time.fromRaw(data.timestamps[i]);
        const x = Math.max(0, calculateX(timestamp));
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
      ctx.lineTo(endPx, lastDrawnY);
      ctx.lineTo(endPx, zeroY);
      ctx.closePath();
      ctx.fill();
      ctx.stroke();
    }

    // Draw CPU idle rectangles that overlay the CPU freq graph.
    ctx.fillStyle = `rgba(240, 240, 240, 1)`;
    {
      for (let i = startIdx; i < endIdx; i++) {
        if (data.lastIdleValues[i] < 0) {
          continue;
        }

        // We intentionally don't use the floor function here when computing x
        // coordinates. Instead we use floating point which prevents flickering as
        // we pan and zoom; this relies on the browser anti-aliasing pixels
        // correctly.
        const timestamp = Time.fromRaw(data.timestamps[i]);
        const x = visibleTimeScale.timeToPx(timestamp);
        const xEnd =
          i === data.lastIdleValues.length - 1
            ? endPx
            : visibleTimeScale.timeToPx(Time.fromRaw(data.timestamps[i + 1]));

        const width = xEnd - x;
        const height = calculateY(data.lastFreqKHz[i]) - zeroY;

        ctx.fillRect(x, zeroY, width, height);
      }
    }

    ctx.font = '10px Roboto Condensed';

    if (this.hoveredValue !== undefined && this.hoveredTs !== undefined) {
      let text = `${this.hoveredValue.toLocaleString()}kHz`;

      ctx.fillStyle = color.setHSL({s: 45, l: 75}).cssString;
      ctx.strokeStyle = color.setHSL({s: 45, l: 45}).cssString;

      const xStart = Math.floor(visibleTimeScale.timeToPx(this.hoveredTs));
      const xEnd =
        this.hoveredTsEnd === undefined
          ? endPx
          : Math.floor(visibleTimeScale.timeToPx(this.hoveredTsEnd));
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
        xStart,
        y,
        3 /* r*/,
        0 /* start angle*/,
        2 * Math.PI /* end angle*/,
      );
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
      0,
      size.width,
      visibleTimeScale.timeToPx(data.start),
      visibleTimeScale.timeToPx(data.end),
    );
  }

  onMouseMove(pos: {x: number; y: number}) {
    const data = this.fetcher.data;
    if (data === undefined) return;
    this.mousePos = pos;
    const {visibleTimeScale} = globals.timeline;
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
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const {engine} = ctx;

    const cpus = ctx.trace.cpus;

    const maxCpuFreqResult = await engine.query(`
      select ifnull(max(value), 0) as freq
      from counter c
      join cpu_counter_track t on c.track_id = t.id
      join _counter_track_summary s on t.id = s.id
      where name = 'cpufreq';
    `);
    const maxCpuFreq = maxCpuFreqResult.firstRow({freq: NUM}).freq;

    for (const cpu of cpus) {
      // Only add a cpu freq track if we have cpu freq data.
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
        join _counter_track_summary using (id)
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

        const config = {
          cpu,
          maximumValue: maxCpuFreq,
          freqTrackId,
          idleTrackId,
        };

        ctx.registerTrack({
          uri: `perfetto.CpuFreq#${cpu}`,
          displayName: `Cpu ${cpu} Frequency`,
          kind: CPU_FREQ_TRACK_KIND,
          cpu,
          trackFactory: () => new CpuFreqTrack(config, ctx.engine),
        });
      }
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.CpuFreq',
  plugin: CpuFreq,
};
