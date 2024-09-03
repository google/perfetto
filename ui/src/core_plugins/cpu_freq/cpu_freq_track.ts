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
import {drawTrackHoverTooltip} from '../../base/canvas_utils';
import {colorForCpu} from '../../core/colorizer';
import {TrackData} from '../../common/track_data';
import {TimelineFetcher} from '../../common/track_helper';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {Engine} from '../../trace_processor/engine';
import {Track} from '../../public/track';
import {LONG, NUM} from '../../trace_processor/query_result';
import {uuidv4Sql} from '../../base/uuid';
import {TrackMouseEvent, TrackRenderContext} from '../../public/track';
import {Point2D} from '../../base/geom';
import {createView, createVirtualTable} from '../../trace_processor/sql_utils';
import {AsyncDisposableStack} from '../../base/disposable_stack';

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

export class CpuFreqTrack implements Track {
  private mousePos: Point2D = {x: 0, y: 0};
  private hoveredValue: number | undefined = undefined;
  private hoveredTs: time | undefined = undefined;
  private hoveredTsEnd: time | undefined = undefined;
  private hoveredIdle: number | undefined = undefined;
  private fetcher = new TimelineFetcher<Data>(this.onBoundsChange.bind(this));

  private engine: Engine;
  private config: Config;
  private trackUuid = uuidv4Sql();

  private trash!: AsyncDisposableStack;

  constructor(config: Config, engine: Engine) {
    this.config = config;
    this.engine = engine;
  }

  async onCreate() {
    this.trash = new AsyncDisposableStack();
    if (this.config.idleTrackId === undefined) {
      this.trash.use(
        await createView(
          this.engine,
          `raw_freq_idle_${this.trackUuid}`,
          `
            select ts, dur, value as freqValue, -1 as idleValue
            from experimental_counter_dur c
            where track_id = ${this.config.freqTrackId}
          `,
        ),
      );
    } else {
      this.trash.use(
        await createView(
          this.engine,
          `raw_freq_${this.trackUuid}`,
          `
            select ts, dur, value as freqValue
            from experimental_counter_dur c
            where track_id = ${this.config.freqTrackId}
          `,
        ),
      );

      this.trash.use(
        await createView(
          this.engine,
          `raw_idle_${this.trackUuid}`,
          `
            select
              ts,
              dur,
              iif(value = 4294967295, -1, cast(value as int)) as idleValue
            from experimental_counter_dur c
            where track_id = ${this.config.idleTrackId}
          `,
        ),
      );

      this.trash.use(
        await createVirtualTable(
          this.engine,
          `raw_freq_idle_${this.trackUuid}`,
          `span_join(raw_freq_${this.trackUuid}, raw_idle_${this.trackUuid})`,
        ),
      );
    }

    this.trash.use(
      await createVirtualTable(
        this.engine,
        `cpu_freq_${this.trackUuid}`,
        `
          __intrinsic_counter_mipmap((
            select ts, freqValue as value
            from raw_freq_idle_${this.trackUuid}
          ))
        `,
      ),
    );

    this.trash.use(
      await createVirtualTable(
        this.engine,
        `cpu_idle_${this.trackUuid}`,
        `
          __intrinsic_counter_mipmap((
            select ts, idleValue as value
            from raw_freq_idle_${this.trackUuid}
          ))
        `,
      ),
    );
  }

  async onUpdate({
    visibleWindow,
    resolution,
  }: TrackRenderContext): Promise<void> {
    await this.fetcher.requestData(visibleWindow.toTimeSpan(), resolution);
  }

  async onDestroy(): Promise<void> {
    await this.trash.asyncDispose();
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

  render({ctx, size, timescale, visibleWindow}: TrackRenderContext): void {
    // TODO: fonts and colors should come from the CSS and not hardcoded here.
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
      return Math.floor(timescale.timeToPx(timestamp));
    };
    const calculateY = (value: number) => {
      return zeroY - Math.round((value / yMax) * RECT_HEIGHT);
    };

    const timespan = visibleWindow.toTimeSpan();
    const start = timespan.start;
    const end = timespan.end;

    const [rawStartIdx] = searchSegment(data.timestamps, start);
    const startIdx = rawStartIdx === -1 ? 0 : rawStartIdx;

    const [, rawEndIdx] = searchSegment(data.timestamps, end);
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
        const x = timescale.timeToPx(timestamp);
        const xEnd =
          i === data.lastIdleValues.length - 1
            ? endPx
            : timescale.timeToPx(Time.fromRaw(data.timestamps[i + 1]));

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

      const xStart = Math.floor(timescale.timeToPx(this.hoveredTs));
      const xEnd =
        this.hoveredTsEnd === undefined
          ? endPx
          : Math.floor(timescale.timeToPx(this.hoveredTsEnd));
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
      drawTrackHoverTooltip(ctx, this.mousePos, size, text);
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
      timescale.timeToPx(data.start),
      timescale.timeToPx(data.end),
    );
  }

  onMouseMove({x, y, timescale}: TrackMouseEvent) {
    const data = this.fetcher.data;
    if (data === undefined) return;
    this.mousePos = {x, y};
    const time = timescale.pxToHpTime(x);

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
