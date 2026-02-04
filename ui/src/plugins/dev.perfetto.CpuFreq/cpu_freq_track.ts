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
import {Point2D} from '../../base/geom';
import {assertTrue} from '../../base/logging';
import {Monitor} from '../../base/monitor';
import {duration, time, Time} from '../../base/time';
import {colorForCpu} from '../../components/colorizer';
import m from 'mithril';
import {TrackData} from '../../components/tracks/track_data';
import {TimelineFetcher} from '../../components/tracks/track_helper';
import {checkerboardExcept} from '../../components/checkerboard';
import {TrackRenderer} from '../../public/track';
import {LONG, NUM} from '../../trace_processor/query_result';
import {uuidv4Sql} from '../../base/uuid';
import {TrackMouseEvent, TrackRenderContext} from '../../public/track';
import {TimeScale} from '../../base/time_scale';
import {
  createPerfettoTable,
  createView,
  createVirtualTable,
} from '../../trace_processor/sql_utils';
import {AsyncDisposableStack} from '../../base/disposable_stack';
import {Trace} from '../../public/trace';
import {Color} from '../../base/color';
import {deferChunkedTask} from '../../base/chunked_task';
import {StepAreaBuffers} from '../../base/renderer';

export interface Data extends TrackData {
  timestamps: BigInt64Array;
  minFreqKHz: Uint32Array;
  maxFreqKHz: Uint32Array;
  lastFreqKHz: Uint32Array;
  lastIdleValues: Int8Array;
  // Pre-built buffers for step area rendering.
  // xs: relative timestamps in ns (multiply by pxPerNs and add baseOffsetPx)
  // ys: frequency values in kHz (apply Y transform)
  // minYs/maxYs: min/max freq values for wiggle
  // fills: 1.0 when not idle, 0.0 when idle
  stepAreaBuffers: StepAreaBuffers;
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

interface CpuFreqHover {
  ts: time;
  tsEnd?: time;
  value: number;
  idle: number;
}

function computeHover(
  pos: Point2D | undefined,
  timescale: TimeScale,
  data: Data,
): CpuFreqHover | undefined {
  if (pos === undefined) return undefined;

  const time = timescale.pxToHpTime(pos.x);
  const [left, right] = searchSegment(data.timestamps, time.toTime());
  if (left === -1) return undefined;

  return {
    ts: Time.fromRaw(data.timestamps[left]),
    tsEnd: right === -1 ? undefined : Time.fromRaw(data.timestamps[right]),
    value: data.lastFreqKHz[left],
    idle: data.lastIdleValues[left],
  };
}

export class CpuFreqTrack implements TrackRenderer {
  private hover?: CpuFreqHover;
  private fetcher = new TimelineFetcher<Data>(this.onBoundsChange.bind(this));

  private trackUuid = uuidv4Sql();

  private trash!: AsyncDisposableStack;

  // Cached color for this CPU (constant for track lifetime).
  private readonly color: Color;

  // Monitor for local hover state (triggers DOM redraw for tooltip).
  private readonly hoverMonitor = new Monitor([
    () => this.hover?.ts,
    () => this.hover?.value,
    () => this.hover?.idle,
  ]);

  constructor(
    private readonly config: Config,
    private readonly trace: Trace,
  ) {
    this.color = colorForCpu(this.config.cpu);
  }

  async onCreate() {
    this.trash = new AsyncDisposableStack();
    await this.trace.engine.query(`
      INCLUDE PERFETTO MODULE counters.intervals;
    `);
    if (this.config.idleTrackId === undefined) {
      this.trash.use(
        await createView({
          engine: this.trace.engine,
          name: `raw_freq_idle_${this.trackUuid}`,
          as: `
            select ts, dur, value as freqValue, -1 as idleValue
            from counter_leading_intervals!((
              select id, ts, track_id, value
              from counter
              where track_id = ${this.config.freqTrackId}
            ))
          `,
        }),
      );
    } else {
      this.trash.use(
        await createPerfettoTable({
          engine: this.trace.engine,
          name: `raw_freq_${this.trackUuid}`,
          as: `
            select ts, dur, value as freqValue
            from counter_leading_intervals!((
              select id, ts, track_id, value
              from counter
             where track_id = ${this.config.freqTrackId}
            ))
          `,
        }),
      );

      this.trash.use(
        await createPerfettoTable({
          engine: this.trace.engine,
          name: `raw_idle_${this.trackUuid}`,
          as: `
            select
              ts,
              dur,
              iif(value = 4294967295, -1, cast(value as int)) as idleValue
            from counter_leading_intervals!((
              select id, ts, track_id, value
              from counter
              where track_id = ${this.config.idleTrackId}
            ))
          `,
        }),
      );

      this.trash.use(
        await createVirtualTable({
          engine: this.trace.engine,
          name: `raw_freq_idle_${this.trackUuid}`,
          using: `span_join(raw_freq_${this.trackUuid}, raw_idle_${this.trackUuid})`,
        }),
      );
    }

    this.trash.use(
      await createVirtualTable({
        engine: this.trace.engine,
        name: `cpu_freq_${this.trackUuid}`,
        using: `
        __intrinsic_counter_mipmap((
          select ts, freqValue as value
          from raw_freq_idle_${this.trackUuid}
        ))
      `,
      }),
    );

    this.trash.use(
      await createVirtualTable({
        engine: this.trace.engine,
        name: `cpu_idle_${this.trackUuid}`,
        using: `
        __intrinsic_counter_mipmap((
          select ts, idleValue as value
          from raw_freq_idle_${this.trackUuid}
        ))
      `,
      }),
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

    const freqResult = await this.trace.engine.query(`
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
    const idleResult = await this.trace.engine.query(`
      SELECT last_value as lastIdle
      FROM cpu_idle_${this.trackUuid}(
        ${start},
        ${end},
        ${resolution}
      );
    `);

    const task = await deferChunkedTask();

    const freqRows = freqResult.numRows();
    const idleRows = idleResult.numRows();
    assertTrue(freqRows == idleRows);

    // Allocate arrays for Data and StepAreaBuffers
    const timestamps = new BigInt64Array(freqRows);
    const minFreqKHz = new Uint32Array(freqRows);
    const maxFreqKHz = new Uint32Array(freqRows);
    const lastFreqKHz = new Uint32Array(freqRows);
    const lastIdleValues = new Int8Array(freqRows);

    // StepAreaBuffers arrays (raw data values, transform applied at render time)
    const xs = new Float32Array(freqRows); // Relative timestamps in ns
    const xnext = new Float32Array(freqRows); // Next relative timestamp in ns
    const ys = new Float32Array(freqRows); // Frequency values in kHz
    const minYs = new Float32Array(freqRows); // Max freq (higher value = lower Y after transform)
    const maxYs = new Float32Array(freqRows); // Min freq (lower value = higher Y after transform)
    const fills = new Float32Array(freqRows); // 1.0 when not idle, 0.0 when idle

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
      if (i % 50 === 0 && task.shouldYield()) {
        await task.yield();
      }

      timestamps[i] = freqIt.ts;
      minFreqKHz[i] = freqIt.minFreq;
      maxFreqKHz[i] = freqIt.maxFreq;
      lastFreqKHz[i] = freqIt.lastFreq;
      lastIdleValues[i] = idleIt.lastIdle;

      // Populate step area buffers with raw values
      const x = Number(freqIt.ts - start);
      xs[i] = Math.max(0, x); // Clamp to the start of the frame
      ys[i] = freqIt.lastFreq;

      fills[i] = idleIt.lastIdle < 0 ? 1.0 : 0.0;
      if (i > 0) {
        xnext[i - 1] = x;
        const yprev = ys[i - 1];
        minYs[i] = Math.min(freqIt.minFreq, yprev);
        maxYs[i] = Math.max(freqIt.maxFreq, yprev);
      } else {
        minYs[i] = freqIt.minFreq;
        maxYs[i] = freqIt.maxFreq;
      }
    }

    // The final xnext extends to the end of the frame
    xnext[freqRows - 1] = Number(end - start);

    const data: Data = {
      start,
      end,
      resolution,
      length: freqRows,
      timestamps,
      minFreqKHz,
      maxFreqKHz,
      lastFreqKHz,
      lastIdleValues,
      stepAreaBuffers: {
        xs,
        xnext,
        ys,
        minYs,
        maxYs,
        fillAlpha: fills,
        count: freqRows,
      },
    };

    return data;
  }

  getHeight() {
    return MARGIN_TOP + RECT_HEIGHT;
  }

  renderTooltip(): m.Children {
    if (this.hover === undefined) {
      return undefined;
    }

    let text = `${this.hover.value.toLocaleString()}kHz`;

    // Display idle value if current hover is idle.
    if (this.hover.idle !== -1) {
      // Display the idle value +1 to be consistent with catapult.
      text += ` (Idle: ${(this.hover.idle + 1).toLocaleString()})`;
    }

    return text;
  }

  render({ctx, size, timescale, colors, renderer}: TrackRenderContext): void {
    // TODO: fonts and colors should come from the CSS and not hardcoded here.
    const data = this.fetcher.data;
    if (!data) return;

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

    let saturation = 45;
    if (this.trace.timeline.hoveredUtid !== undefined) {
      saturation = 0;
    }

    const fillColor = this.color.setHSL({s: saturation, l: 50}).setAlpha(0.6);

    // Build transform for converting raw data to screen coordinates.
    // X: screenX = x * pxPerNs + baseOffsetPx (ns -> pixels)
    // Y: screenY = y * (-RECT_HEIGHT / yMax) + zeroY (kHz -> pixels, inverted)
    const pxPerNs = timescale.durationToPx(1n);
    const baseOffsetPx = timescale.timeToPx(data.start);
    const transform = {
      offsetX: baseOffsetPx,
      scaleX: pxPerNs,
      offsetY: zeroY,
      scaleY: -RECT_HEIGHT / yMax,
    };

    renderer.drawStepArea(
      data.stepAreaBuffers,
      transform,
      fillColor,
      MARGIN_TOP,
      MARGIN_TOP + RECT_HEIGHT,
    );
    renderer.flush();

    ctx.font = '10px Roboto Condensed';

    if (this.hover !== undefined) {
      ctx.fillStyle = this.color.setHSL({s: 45, l: 75}).cssString;
      ctx.strokeStyle = this.color.setHSL({s: 45, l: 45}).cssString;

      const hoverRelNs = Number(this.hover.ts) - Number(data.start);
      const xStart = Math.floor(hoverRelNs * pxPerNs + baseOffsetPx);
      const xEnd =
        this.hover.tsEnd === undefined
          ? endPx
          : Math.floor(
              (Number(this.hover.tsEnd) - Number(data.start)) * pxPerNs +
                baseOffsetPx,
            );
      const y = zeroY - Math.round((this.hover.value / yMax) * RECT_HEIGHT);

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
    }

    // Write the Y scale on the top left corner.
    ctx.textBaseline = 'alphabetic';
    ctx.fillStyle = colors.COLOR_BACKGROUND;
    ctx.globalAlpha = 0.6;
    ctx.fillRect(0, 0, 42, 18);
    ctx.globalAlpha = 1;
    ctx.fillStyle = colors.COLOR_TEXT;
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
    this.hover = computeHover({x, y}, timescale, data);
    if (this.hoverMonitor.ifStateChanged()) {
      this.trace.raf.scheduleFullRedraw();
    }
  }

  onMouseOut() {
    this.hover = undefined;
    if (this.hoverMonitor.ifStateChanged()) {
      this.trace.raf.scheduleFullRedraw();
    }
  }
}
