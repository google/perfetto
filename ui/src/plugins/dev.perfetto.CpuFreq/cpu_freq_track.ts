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

import {searchSorted} from '../../base/binary_search';
import {Point2D} from '../../base/geom';
import {assertTrue} from '../../base/logging';
import {Monitor} from '../../base/monitor';
import {Time, time} from '../../base/time';
import {colorForCpu} from '../../components/colorizer';
import m from 'mithril';
import {checkerboardExcept} from '../../components/checkerboard';
import {CacheKey} from '../../components/tracks/timeline_cache';
import {TrackRenderPipeline} from '../../components/tracks/track_render_pipeline';
import {TrackUpdateContext, TrackRenderer} from '../../public/track';
import {LONG, NUM, Row} from '../../trace_processor/query_result';
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

// Row spec for the freq mipmap query.
const FREQ_ROW = {
  ts: LONG,
  minFreq: NUM,
  maxFreq: NUM,
  lastFreq: NUM,
};

// Row spec for the idle mipmap query.
const IDLE_ROW = {
  lastIdle: NUM,
};

// Entry stored in the freq pipeline buffer.
interface FreqEntry {
  ts: time;
  minFreqKHz: number;
  maxFreqKHz: number;
  lastFreqKHz: number;
}

// Entry stored in the idle pipeline buffer.
interface IdleEntry {
  lastIdleValue: number;
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
  freqData: FreqEntry[] | undefined,
  idleData: IdleEntry[] | undefined,
): CpuFreqHover | undefined {
  if (pos === undefined) return undefined;
  if (freqData === undefined || freqData.length === 0) return undefined;

  const targetTime = timescale.pxToHpTime(pos.x).toTime();
  const idx = searchSorted(freqData, targetTime, (e) => e.ts);
  if (idx === -1) return undefined;

  return {
    ts: freqData[idx].ts,
    tsEnd: idx + 1 < freqData.length ? freqData[idx + 1].ts : undefined,
    value: freqData[idx].lastFreqKHz,
    idle: idleData?.[idx]?.lastIdleValue ?? -1,
  };
}

export class CpuFreqTrack implements TrackRenderer {
  private hover?: CpuFreqHover;

  private trackUuid = uuidv4Sql();
  private cacheKey = CacheKey.zero();

  private trash!: AsyncDisposableStack;

  // Separate pipelines for freq and idle data to avoid JOIN issues with
  // table-valued functions.
  private freqPipeline?: TrackRenderPipeline<
    Row & typeof FREQ_ROW,
    FreqEntry,
    object
  >;
  private idlePipeline?: TrackRenderPipeline<
    Row & typeof IDLE_ROW,
    IdleEntry,
    object
  >;

  // Monitor for local hover state (triggers DOM redraw for tooltip).
  private readonly hoverMonitor = new Monitor([
    () => this.hover?.ts,
    () => this.hover?.value,
    () => this.hover?.idle,
  ]);

  constructor(
    private readonly config: Config,
    private readonly trace: Trace,
  ) {}

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

    // Initialize the pipelines separately to avoid JOIN issues with
    // table-valued functions.
    this.freqPipeline = new TrackRenderPipeline(
      this.trace,
      (_rawSql: string, key: CacheKey) => `
        SELECT
          min_value as minFreq,
          max_value as maxFreq,
          last_ts as ts,
          last_value as lastFreq
        FROM cpu_freq_${this.trackUuid}(${key.start}, ${key.end}, ${key.bucketSize})
      `,
      () => ({}),
      (row, _state) => ({
        ts: Time.fromRaw(row.ts),
        minFreqKHz: row.minFreq,
        maxFreqKHz: row.maxFreq,
        lastFreqKHz: row.lastFreq,
      }),
    );

    this.idlePipeline = new TrackRenderPipeline(
      this.trace,
      (_rawSql: string, key: CacheKey) => `
        SELECT last_value as lastIdle
        FROM cpu_idle_${this.trackUuid}(${key.start}, ${key.end}, ${key.bucketSize})
      `,
      () => ({}),
      (row, _state) => ({
        lastIdleValue: row.lastIdle,
      }),
    );
  }

  async onUpdate(ctx: TrackUpdateContext): Promise<void> {
    if (this.freqPipeline === undefined || this.idlePipeline === undefined) {
      return;
    }

    // Run both pipelines. They use the same cache key parameters so rows
    // should align by index.
    const [freqResult, idleResult] = await Promise.all([
      this.freqPipeline.onUpdate('', FREQ_ROW, ctx),
      this.idlePipeline.onUpdate('', IDLE_ROW, ctx),
    ]);

    if (freqResult === 'updated' || idleResult === 'updated') {
      this.cacheKey = this.freqPipeline.getCacheKey();
    }
  }

  async onDestroy(): Promise<void> {
    await this.trash.asyncDispose();
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

  render({
    ctx,
    size,
    timescale,
    visibleWindow,
    colors,
  }: TrackRenderContext): void {
    // TODO: fonts and colors should come from the CSS and not hardcoded here.
    const freqData = this.freqPipeline?.getActiveBuffer() ?? [];
    const idleData = this.idlePipeline?.getActiveBuffer() ?? [];

    if (freqData.length === 0) {
      // Can't possibly draw anything.
      return;
    }

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
    if (this.trace.timeline.hoveredUtid !== undefined) {
      saturation = 0;
    }

    ctx.fillStyle = color
      .setHSL({s: saturation, l: 50})
      .setAlpha(0.6).cssString;
    ctx.strokeStyle = color.setHSL({s: saturation, l: 50}).cssString;

    const calculateX = (timestamp: time) => {
      return Math.floor(timescale.timeToPx(timestamp));
    };
    const calculateY = (value: number) => {
      return zeroY - Math.round((value / yMax) * RECT_HEIGHT);
    };

    // Find the visible range of data points.
    const timespan = visibleWindow.toTimeSpan();
    const rawStartIdx = searchSorted(freqData, timespan.start, (e) => e.ts);
    const startIdx = rawStartIdx === -1 ? 0 : rawStartIdx;

    const rawEndIdx = searchSorted(freqData, timespan.end, (e) => e.ts);
    const endIdx = rawEndIdx === -1 ? 0 : rawEndIdx + 1;

    // Draw the CPU frequency graph.
    {
      ctx.beginPath();
      ctx.moveTo(Math.max(calculateX(freqData[startIdx].ts), 0), zeroY);

      let lastDrawnY = zeroY;
      for (let i = startIdx; i < endIdx; i++) {
        const entry = freqData[i];
        const x = Math.max(0, calculateX(entry.ts));
        const minY = calculateY(entry.minFreqKHz);
        const maxY = calculateY(entry.maxFreqKHz);
        const lastY = calculateY(entry.lastFreqKHz);

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
    ctx.fillStyle = `rgba(128,128,128, 0.2)`;
    {
      for (let i = startIdx; i < endIdx; i++) {
        const freqEntry = freqData[i];
        const idleValue = idleData[i]?.lastIdleValue ?? -1;
        if (idleValue < 0) {
          continue;
        }

        // We intentionally don't use the floor function here when computing x
        // coordinates. Instead we use floating point which prevents flickering as
        // we pan and zoom; this relies on the browser anti-aliasing pixels
        // correctly.
        const x = timescale.timeToPx(freqEntry.ts);
        const xEnd =
          i === freqData.length - 1
            ? endPx
            : timescale.timeToPx(freqData[i + 1].ts);

        const width = xEnd - x;
        const height = calculateY(freqEntry.lastFreqKHz) - zeroY;

        ctx.clearRect(x, zeroY, width, height);
        ctx.fillRect(x, zeroY, width, height);
      }
    }

    ctx.font = '10px Roboto Condensed';

    if (this.hover !== undefined) {
      ctx.fillStyle = color.setHSL({s: 45, l: 75}).cssString;
      ctx.strokeStyle = color.setHSL({s: 45, l: 45}).cssString;

      const xStart = Math.floor(timescale.timeToPx(this.hover.ts));
      const xEnd =
        this.hover.tsEnd === undefined
          ? endPx
          : Math.floor(timescale.timeToPx(this.hover.tsEnd));
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
      timescale.timeToPx(this.cacheKey.start),
      timescale.timeToPx(this.cacheKey.end),
    );
  }

  onMouseMove({x, y, timescale}: TrackMouseEvent) {
    const freqData = this.freqPipeline?.getActiveBuffer();
    const idleData = this.idlePipeline?.getActiveBuffer();
    this.hover = computeHover({x, y}, timescale, freqData, idleData);
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
