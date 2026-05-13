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
import {valueIfAllEqual} from '../../base/array_utils';
import {assertUnreachable} from '../../base/assert';
import {searchSegment} from '../../base/binary_search';
import {deferChunkedTask} from '../../base/chunked_task';
import {HSLColor} from '../../base/color';
import {Point2D} from '../../base/geom';
import {formatNumber} from '../../base/number_format';
import {
  CancellationSignal,
  QUERY_CANCELLED,
  QuerySlot,
  SerialTaskQueue,
} from '../../base/query_slot';
import {Icons} from '../../base/semantic_icons';
import {duration, time} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import {Trace} from '../../public/trace';
import {
  TrackMouseEvent,
  TrackRenderContext,
  TrackRenderer,
  TrackSetting,
  TrackSettingDescriptor,
} from '../../public/track';
import {NUM} from '../../trace_processor/query_result';
import {
  createPerfettoTable,
  createVirtualTable,
} from '../../trace_processor/sql_utils';
import {MenuItem} from '../../widgets/menu';
import {checkerboardExcept} from '../checkerboard';
import {BufferedBounds} from './buffered_bounds';
import {CHUNKED_TASK_BACKGROUND_PRIORITY} from './feature_flags';
import {RangeSharer} from './range_sharer';

export type ChartHeightSize = 1 | 2 | 4 | 8 | 16 | 32;
export type YMode = 'value' | 'delta' | 'rate';
export type YRange = 'all' | 'viewport';

interface Limits {
  readonly min: number;
  readonly max: number;
}

interface MipmapTableResult extends AsyncDisposable {
  readonly tableName: string; // The name of the virtual table containing the mipmap data which can be used in SQL queries.
  readonly globalLimits: Limits; // Min and max values across the entire trace.
}

interface DataFrame {
  readonly start: time; // Start time of the dataframe
  readonly end: time; // End time of the dataframe
  readonly limits: Limits; // Min and max values within the dataframe
  readonly timestampsRel: Float32Array; // Timestamps relative to dataStart
  readonly timestampsRelNext: Float32Array; // Timestamps of next sample relative to dataStart
  readonly minDisplayValues: Float32Array; // Min value within each bucket
  readonly maxDisplayValues: Float32Array; // Max value within each bucket
  readonly lastDisplayValues: Float32Array; // Final value within each bucket
}

interface HoverState {
  readonly lastDisplayValue: number;
  readonly tsRel: number;
  readonly tsEndRel?: number;
}

const BUCKETS_PER_PIXEL = 2;
const TRACK_HEIGHT_PX = 40;
const TRACK_PADDING = 2; // px gap between waveform peak/trough and track edge

const CHART_HEIGHT_LABELS: [string, ChartHeightSize][] = [
  ['Small (1x)', 1],
  ['Medium (2x)', 2],
  ['Large (4x)', 4],
  ['XLarge (8x)', 8],
  ['XXLarge (16x)', 16],
  ['XXXLarge (32x)', 32],
];
const yModeDescriptor: TrackSettingDescriptor<YMode> = {
  name: 'Y Mode',
  description: 'TODO',
  render(setter, values) {
    const value = valueIfAllEqual(values);
    return m(MenuItem, {label: `Mode (currently: ${value ?? 'mixed'})`}, [
      m(MenuItem, {
        label: 'Value',
        onclick: () => setter('value'),
        icon: value === 'value' ? Icons.RadioChecked : Icons.RadioUnchecked,
      }),
      m(MenuItem, {
        label: 'Delta',
        onclick: () => setter('delta'),
        icon: value === 'delta' ? Icons.RadioChecked : Icons.RadioUnchecked,
      }),
      m(MenuItem, {
        label: 'Rate',
        onclick: () => setter('rate'),
        icon: value === 'rate' ? Icons.RadioChecked : Icons.RadioUnchecked,
      }),
    ]);
  },
};

const yRangeSettingDescriptor: TrackSettingDescriptor<YRange> = {
  name: 'Y-axis range',
  description: 'all, viewport',
  render(setter, values) {
    const value = valueIfAllEqual(values);

    const icon = (() => {
      switch (value) {
        case 'viewport':
          return 'check_box';
        case 'all':
          return 'check_box_outline_blank';
        default:
          return 'indeterminate_check_box';
      }
    })();

    return m(MenuItem, {
      label: 'Zoom on scroll',
      icon,
      onclick: () => {
        switch (value) {
          case 'all':
            setter('viewport');
            break;
          case 'viewport':
          default:
            setter('all');
            break;
        }
      },
    });
  },
};

const yDisplayDescriptor: TrackSettingDescriptor<'zero' | 'minmax' | 'log'> = {
  name: 'Y-axis display',
  description: 'zero, minmax, log',
  render(setter, values) {
    const value = valueIfAllEqual(values);
    return m(MenuItem, {label: `Display (currently: ${value ?? 'mixed'})`}, [
      m(MenuItem, {
        label: 'Zero-based',
        onclick: () => setter('zero'),
        icon: value === 'zero' ? Icons.RadioChecked : Icons.RadioUnchecked,
      }),
      m(MenuItem, {
        label: 'Min/Max',
        onclick: () => setter('minmax'),
        icon: value === 'minmax' ? Icons.RadioChecked : Icons.RadioUnchecked,
      }),
      m(MenuItem, {
        label: 'Log',
        onclick: () => setter('log'),
        icon: value === 'log' ? Icons.RadioChecked : Icons.RadioUnchecked,
      }),
    ]);
  },
};

const yRangeRoundingDescriptor: TrackSettingDescriptor<
  'strict' | 'human_readable'
> = {
  name: 'Y-axis rounding',
  description: 'strict, human_readable',
  render(setter, values) {
    const value = valueIfAllEqual(values);

    const icon = (() => {
      switch (value) {
        case 'human_readable':
          return Icons.Checkbox;
        case 'strict':
          return Icons.BlankCheckbox;
        default:
          return Icons.IndeterminateCheckbox;
      }
    })();

    return m(MenuItem, {
      label: 'Round y-axis scale',
      icon,
      onclick: () => {
        setter(value === 'strict' ? 'human_readable' : 'strict');
      },
    });
  },
};

const chartSizeDescriptor: TrackSettingDescriptor<ChartHeightSize> = {
  name: 'Chart height',
  description: '1, 2, 4, 8, 16, 32',
  render(setter, values) {
    const value = valueIfAllEqual(values);
    return m(MenuItem, {label: `Size (currently: ${value ?? 'mixed'})`}, [
      CHART_HEIGHT_LABELS.map(([label, size]) =>
        m(MenuItem, {
          label,
          onclick: () => setter(size),
          icon: value === size ? Icons.RadioChecked : Icons.RadioUnchecked,
        }),
      ),
    ]);
  },
};

export interface CounterTrackAttrs {
  /** The trace object used to run queries. */
  readonly trace: Trace;

  /** A unique, reproducible ID for this track. */
  readonly uri: string;

  /** SQL source selecting the necessary data (must expose ts and value). */
  readonly sqlSource: string;

  /** Y-axis display mode: 'value' | 'delta' | 'rate'. Defaults to 'value'. */
  readonly yMode?: YMode;

  /** Y-axis range mode: 'all' | 'viewport'. Defaults to 'all'. */
  readonly yRange?: YRange;

  /** Y-axis display style: 'zero' | 'minmax' | 'log'. Defaults to 'zero'. */
  readonly yDisplay?: 'zero' | 'minmax' | 'log';

  /** Y-axis rounding: 'strict' | 'human_readable'. Defaults to 'human_readable'. */
  readonly yRangeRounding?: 'strict' | 'human_readable';

  /** Chart height multiplier. Defaults to 1. */
  readonly chartHeightSize?: ChartHeightSize;

  /** Unit string to display in tooltip and labels. */
  readonly unit?: string;

  /** What to display when yMode is 'rate'. */
  readonly rateUnit?: string;

  /** Override the maximum displayed y value. */
  readonly yOverrideMaximum?: number;

  /** Override the minimum displayed y value. */
  readonly yOverrideMinimum?: number;

  /** Optional key to share y-axis range with other tracks. */
  readonly yRangeSharingKey?: string;

  /** Optional lifecycle callback, called once before the first render. */
  onInit?(): Promise<void>;
}

export class CounterTrack implements TrackRenderer {
  // QuerySlot infrastructure
  private readonly queue = new SerialTaskQueue();
  private readonly initSlot = new QuerySlot<AsyncDisposable | void>(this.queue);
  private readonly tableSlot = new QuerySlot<MipmapTableResult>(this.queue);
  private readonly dataSlot = new QuerySlot<DataFrame>(this.queue);

  // Buffered bounds tracking
  private readonly bufferedBounds = new BufferedBounds();

  protected readonly trace: Trace;
  protected readonly uri: string;
  protected readonly sqlSource: string;
  private readonly rangeSharer: RangeSharer;
  private readonly onInitFn?: () => Promise<void>;

  // Mutable display settings (changed via the settings menu)
  protected yMode: YMode;
  private yRange: YRange;
  private yDisplay: 'zero' | 'minmax' | 'log';
  private yRangeRounding: 'strict' | 'human_readable';
  private chartHeightSize: ChartHeightSize;

  // Immutable display options (set from attrs)
  private readonly _unit?: string;
  private readonly _rateUnit?: string;
  private readonly yOverrideMaximum?: number;
  private readonly yOverrideMinimum?: number;
  private readonly yRangeSharingKey?: string;

  // Reference to latest rendered data for hover computation.
  // Always set from QuerySlot results — never cleared manually.
  private dataframe?: DataFrame;

  // The current hover state cached from the most recent mouse move event.
  private hover?: HoverState;

  constructor(attrs: CounterTrackAttrs) {
    const {
      trace,
      uri,
      sqlSource,
      yMode = 'value',
      yRange = 'all',
      yDisplay = 'zero',
      yRangeRounding = 'human_readable',
      chartHeightSize = 1,
      unit,
      rateUnit,
      yOverrideMaximum,
      yOverrideMinimum,
      yRangeSharingKey,
      onInit,
    } = attrs;
    this.trace = trace;
    this.uri = uri;
    this.sqlSource = sqlSource;
    this.yMode = yMode;
    this.yRange = yRange;
    this.yDisplay = yDisplay;
    this.yRangeRounding = yRangeRounding;
    this.chartHeightSize = chartHeightSize;
    this._unit = unit;
    this._rateUnit = rateUnit;
    this.yOverrideMaximum = yOverrideMaximum;
    this.yOverrideMinimum = yOverrideMinimum;
    this.yRangeSharingKey = yRangeSharingKey;
    this.rangeSharer = RangeSharer.getRangeSharer(trace);
    this.onInitFn = onInit;
  }

  // -- Static factory methods --

  /**
   * Synchronous factory: creates a counter track directly from attrs.
   * The sqlSource is evaluated lazily on each render.
   */
  static create(attrs: CounterTrackAttrs): CounterTrack {
    return new CounterTrack(attrs);
  }

  /**
   * Async factory: materializes the sqlSource into a Perfetto table first,
   * then creates the track using the table name as its source.
   * Prefer this when the underlying query is expensive to re-evaluate.
   */
  static async createMaterialized(
    attrs: CounterTrackAttrs,
  ): Promise<CounterTrack> {
    const table = await createPerfettoTable({
      engine: attrs.trace.engine,
      as: attrs.sqlSource,
    });
    return new CounterTrack({...attrs, sqlSource: table.name});
  }

  // -- Public API --

  get unit(): string {
    return this._unit ?? '';
  }

  get rateUnit(): string {
    return this._rateUnit ?? `\u0394${this.unit}/s`;
  }

  // Expose the available settings for this track. This is an ordered list of
  // settings and their descriptors which is used to render both the single and
  // bulk settings menus for this track. When the bulk settings menu is
  // rendered, settings from different tracks are combined using descriptor
  // reference equality.
  get settings(): ReadonlyArray<TrackSetting> {
    const setting = <T>(x: TrackSetting<T>) => x;

    return [
      setting({
        descriptor: yModeDescriptor,
        value: this.yMode,
        update: (value) => {
          this.yMode = value;
        },
      }),
      setting({
        descriptor: yRangeSettingDescriptor,
        value: this.yRange,
        update: (value) => {
          this.yRange = value;
        },
      }),
      setting({
        descriptor: yDisplayDescriptor,
        value: this.yDisplay,
        update: (value) => {
          this.yDisplay = value;
        },
      }),
      setting({
        descriptor: yRangeRoundingDescriptor,
        value: this.yRangeRounding,
        update: (value) => {
          this.yRangeRounding = value;
        },
      }),
      setting({
        descriptor: chartSizeDescriptor,
        value: this.chartHeightSize,
        update: (value) => {
          this.chartHeightSize = value;
        },
      }),
    ];
  }

  // Returns the height of the track in pixels.
  getHeight() {
    return TRACK_HEIGHT_PX * this.chartHeightSize;
  }

  // Called every render cycle to draw the track to the timeline.
  render(trackCtx: TrackRenderContext): void {
    const {ctx, size, timescale, colors, renderer} = trackCtx;

    // Fetch data declaratively — all state flows through QuerySlot caches
    const result = this.useData(trackCtx);
    if (result === undefined) {
      return; // No data ready yet
    }

    const {limits, counters: data} = result;

    // Keep a reference for hover computation
    this.dataframe = data;

    const {
      timestampsRel,
      timestampsRelNext,
      limits: displayValueRange,
      minDisplayValues,
      maxDisplayValues,
      lastDisplayValues,
      start: dataStart,
    } = data;

    const fillAlpha = new Float32Array(timestampsRel.length).fill(1.0);

    // Choose a range for the y-axis
    const {yMin, yMax} = this.computeYRange(limits, displayValueRange);

    const trackHeight = this.getHeight();
    const endPx = size.width;

    // Use hue to differentiate the scale of the counter value
    const exp = Math.ceil(Math.log10(Math.max(yMax, 1)));
    const expCapped = Math.min(exp - 3, 9);
    const hue = (180 - Math.floor(expCapped * (180 / 6)) + 360) % 360;
    const fillColor = new HSLColor([hue, 45, 50], 0.6);

    // Pre-compute conversion factors for fast timestamp-to-pixel conversion.
    const pxPerNs = timescale.durationToPx(1n);
    const baseOffsetPx = timescale.timeToPx(dataStart);

    const calculateX = (relNs: number) => {
      return Math.floor(relNs * pxPerNs + baseOffsetPx);
    };

    const yRange = yMax - yMin;

    // Suppress padding on the side where zero is the boundary — the baseline
    // sits flush against the edge by design.
    const padTop = yMax === 0 ? 0 : TRACK_PADDING;
    const padBottom = yMin === 0 ? 0 : TRACK_PADDING;

    // The -1 ensures yMin never reaches trackHeight (the first pixel of the
    // next track).
    const drawHeight = trackHeight - padTop - padBottom - 1;
    const zeroY = padTop + drawHeight * (yMax / yRange);

    // Draw the counter graph using the renderer
    const count = timestampsRel.length;
    if (count >= 1) {
      // Build transform: raw data -> screen coordinates
      // X: screenX = relNs * pxPerNs + baseOffsetPx
      // Y: screenY = value * scaleY + offsetY (where y=0 maps to zeroY)
      const transform = {
        offsetX: baseOffsetPx,
        scaleX: pxPerNs,
        offsetY: zeroY,
        scaleY: -drawHeight / yRange,
      };

      renderer.drawStepArea(
        {
          xs: timestampsRel,
          ys: lastDisplayValues,
          minYs: minDisplayValues,
          maxYs: maxDisplayValues,
          xnext: timestampsRelNext,
          fillAlpha,
          count,
        },
        transform,
        fillColor,
        0,
        trackHeight,
      );
    }

    const hover = this.hover;
    if (hover !== undefined) {
      ctx.fillStyle = `hsl(${hue}, 45%, 75%)`;
      ctx.strokeStyle = `hsl(${hue}, 45%, 45%)`;

      // Convert hover timestamps to relative for calculateX
      const rawXStart = calculateX(hover.tsRel);
      const xStart = Math.max(0, rawXStart);
      const xEnd =
        hover.tsEndRel !== undefined ? calculateX(hover.tsEndRel) : endPx;
      const y = Math.round(
        padTop +
          drawHeight -
          ((hover.lastDisplayValue - yMin) / yRange) * drawHeight,
      );

      // Highlight line.
      ctx.beginPath();
      ctx.moveTo(xStart, y);
      ctx.lineTo(xEnd, y);
      ctx.lineWidth = 2;
      ctx.stroke();
      ctx.lineWidth = 1;

      // Draw change marker if it would be visible.
      if (rawXStart >= -6) {
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
    }

    // Write the Y range labels.
    ctx.font = '10px Roboto Condensed';
    ctx.textAlign = 'left';
    ctx.fillStyle = colors.COLOR_TEXT;

    if (yMax !== 0) {
      this.drawLabel(ctx, colors, this.formatYValue(yMax), 0, 0, 'top');
    }

    // Draw the min label as long as it's not 0
    if (yMin !== 0) {
      this.drawLabel(
        ctx,
        colors,
        this.formatYValue(yMin),
        0,
        trackHeight,
        'bottom',
      );
    }

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
    const loadedBounds = this.bufferedBounds.bounds;
    checkerboardExcept(
      ctx,
      this.getHeight(),
      0,
      size.width,
      timescale.timeToPx(loadedBounds.start),
      timescale.timeToPx(loadedBounds.end),
    );
  }

  onMouseMove({x, y, timescale}: TrackMouseEvent) {
    const newHover = this.findSampleAtPos({x, y}, timescale);
    if (JSON.stringify(newHover) !== JSON.stringify(this.hover)) {
      m.redraw();
    }
    this.hover = newHover;
  }

  onMouseOut() {
    if (this.hover) {
      m.redraw();
    }
    this.hover = undefined;
  }

  renderTooltip(): m.Children {
    if (!this.hover) return undefined;
    const text = this.formatYValue(
      this.hover.lastDisplayValue,
      (v, unit) => `${v.toLocaleString()}${unit}`,
    );
    return m('.pf-track__tooltip', text);
  }

  // -- Private methods --

  private findSampleAtPos(
    pos: Point2D,
    timescale: TimeScale,
  ): HoverState | undefined {
    const data = this.dataframe;
    if (!data) return undefined;

    // Convert the screen position to a relative NS offset matching timestampsRel.
    const relNs = Number(timescale.pxToHpTime(pos.x).toTime() - data.start);
    const [left, right] = searchSegment(data.timestampsRel, relNs);
    if (left === -1) return undefined;

    return {
      tsRel: data.timestampsRel[left],
      tsEndRel: right === -1 ? undefined : data.timestampsRel[right],
      lastDisplayValue: data.lastDisplayValues[left],
    };
  }

  private useData(
    trackCtx: TrackRenderContext,
  ): {counters: DataFrame; limits: Limits} | undefined {
    const {size, visibleWindow} = trackCtx;

    // Step 0: Call onInit with a constant key
    const initResult = this.initSlot.use({
      key: {init: true},
      queryFn: () => this.onInitFn?.() ?? Promise.resolve(),
    });

    if (initResult.isPending) {
      return undefined;
    }

    // Step 1: Get the mipmap table (created once per SQL source + options)
    // Include yMode and yDisplay in key since they affect the value expression
    const {yMode, yDisplay} = this;
    const tableResult = this.tableSlot.use({
      key: {
        sqlSource: this.sqlSource,
        yMode,
        yDisplay,
      },
      queryFn: () => this.createMipmapTable(),
    });

    const table = tableResult.data;
    if (table === undefined) return undefined;

    // Step 2: Calculate buffered bounds and fetch counter data
    const visibleSpan = visibleWindow.toTimeSpan();
    const windowSizePx = Math.max(1, size.width);
    const bucketSize = this.computeBucketSize(
      visibleSpan.duration,
      windowSizePx,
    );
    const bounds = this.bufferedBounds.update(visibleSpan, bucketSize);

    // Step 3: Fetch counter data using QuerySlot
    const queryStart = bounds.start;
    const queryEnd = bounds.end;
    const dataResult = this.dataSlot.use({
      key: {
        start: bounds.start,
        end: bounds.end,
        resolution: bounds.resolution,
        yMode,
        yDisplay,
      },
      queryFn: async (signal) => {
        return await this.trace.taskTracker.track(
          this.fetchCounterData(
            table.tableName,
            queryStart,
            queryEnd,
            bounds.resolution,
            signal,
          ),
          'Loading counters',
        );
      },
      retainOn: ['start', 'end', 'resolution'],
    });

    const counters = dataResult.data;
    if (counters === undefined) return undefined;

    return {counters, limits: table.globalLimits};
  }

  // Compute the range of values to display and range label.
  private computeYRange(
    // Global min/max across the entire counter track (all data).
    globalLimits: Limits,
    // Min/max of display values in the currently visible viewport.
    viewportLimits: Limits,
  ): {
    yMin: number;
    yMax: number;
  } {
    const {
      yRange,
      yDisplay,
      yRangeRounding,
      yOverrideMaximum: overrideYMax,
      yOverrideMinimum: overrideYMin,
    } = this;

    let yMin = globalLimits.min;
    let yMax = globalLimits.max;

    if (yRange === 'viewport') {
      const {min, max} = viewportLimits;
      yMin = min;
      yMax = max;
    }

    if (yDisplay === 'zero') {
      yMin = Math.min(0, yMin);
      yMax = Math.max(0, yMax);
    }

    if (overrideYMax !== undefined) {
      yMax = Math.max(overrideYMax, yMax);
    }

    if (overrideYMin !== undefined) {
      yMin = Math.min(overrideYMin, yMin);
    }

    if (yRangeRounding === 'human_readable') {
      if (yDisplay === 'log') {
        yMax = Math.log(roundUp(Math.exp(yMax)));
        yMin = Math.log(roundDown(Math.exp(yMin)));
      } else {
        yMax = roundUp(yMax);
        yMin = roundDown(yMin);
      }
    }

    [yMin, yMax] = this.rangeSharer.share(
      {
        yRangeSharingKey: this.yRangeSharingKey,
        yMode: this.yMode,
        yDisplay: this.yDisplay,
        chartHeightSize: this.chartHeightSize,
      },
      [yMin, yMax],
    );

    return {
      yMin,
      yMax,
    };
  }

  private drawLabel(
    ctx: CanvasRenderingContext2D,
    colors: TrackRenderContext['colors'],
    text: string,
    x: number,
    y: number,
    baseline: CanvasTextBaseline,
  ): void {
    const pad = 2;
    const leftPad = 4;
    ctx.textBaseline = baseline;
    const metrics = ctx.measureText(text);
    const textHeight =
      metrics.actualBoundingBoxAscent + metrics.actualBoundingBoxDescent;
    const rectY = y - metrics.actualBoundingBoxAscent - pad;
    ctx.fillStyle = colors.COLOR_BACKGROUND;
    ctx.globalAlpha = 0.6;
    ctx.fillRect(
      0,
      rectY,
      leftPad +
        metrics.actualBoundingBoxLeft +
        metrics.actualBoundingBoxRight +
        pad,
      textHeight + 2 * pad,
    );
    ctx.fillStyle = colors.COLOR_TEXT;
    ctx.globalAlpha = 1;
    ctx.fillText(text, x + leftPad, y);
  }

  private formatYValue(
    value: number,
    fmt: (v: number, unit: string) => string = formatNumber,
  ): string {
    const {yMode, yDisplay} = this;
    const v = yDisplay === 'log' ? Math.exp(value) : value;

    switch (yMode) {
      case 'value':
        return fmt(v, this.unit);
      case 'delta':
        return `\u0394${fmt(v, this.unit)}`;
      case 'rate':
        return fmt(v, this.rateUnit);
      default:
        assertUnreachable(yMode);
    }
  }

  // The underlying table has `ts` and `value` columns.
  private getValueExpression(): string {
    const {yMode, yDisplay} = this;
    const valueExpr = counterValueExpression(yMode);

    if (yDisplay === 'log') {
      return `ifnull(ln(${valueExpr}), 0)`;
    } else {
      return valueExpr;
    }
  }

  // Compute bucket size for a given time span and pixel width
  private computeBucketSize(
    spanDuration: duration,
    windowSizePx: number,
  ): duration {
    const nsPerPx = Math.max(1, Number(spanDuration) / windowSizePx);
    const bucketNs = nsPerPx / BUCKETS_PER_PIXEL;
    const exp = Math.ceil(Math.log2(Math.max(1, bucketNs)));
    return BigInt(Math.pow(2, exp)) as duration;
  }

  // Creates the mipmap table - called declaratively from render via QuerySlot
  private async createMipmapTable(): Promise<MipmapTableResult> {
    const table = await createVirtualTable({
      engine: this.engine,
      using: `__intrinsic_counter_mipmap((
        SELECT
          ts,
          ${this.getValueExpression()} AS value
        FROM (${this.sqlSource})
      ))`,
    });

    // Fetch the global limits
    const limitsQuery = await this.engine.query(`
      SELECT
        min_value AS min,
        max_value AS max
      FROM ${table.name}(
        trace_start(), trace_end() + 1, trace_dur() + 1
      );
    `);

    const {min, max} = limitsQuery.firstRow({
      min: NUM,
      max: NUM,
    });

    return {
      tableName: table.name,
      globalLimits: {min, max},
      [Symbol.asyncDispose]: async () => {
        await table[Symbol.asyncDispose]();
      },
    };
  }

  // Fetches counter data for the given bounds - called from QuerySlot
  private async fetchCounterData(
    tableName: string,
    start: time,
    end: time,
    resolution: duration,
    signal: CancellationSignal,
  ): Promise<DataFrame> {
    const queryRes = await this.engine.query(`
      SELECT
        min_value AS minDisplayValue,
        max_value AS maxDisplayValue,
        MAX(0, MIN(last_ts - ${start}, ${end} - ${start})) AS tsRel,
        last_value AS lastDisplayValue
      FROM ${tableName}(
        ${start},
        ${end},
        ${resolution}
      );
    `);

    if (signal.isCancelled) throw QUERY_CANCELLED;

    const priority = CHUNKED_TASK_BACKGROUND_PRIORITY.get()
      ? 'background'
      : undefined;
    const task = await deferChunkedTask({priority});

    const it = queryRes.iter({
      tsRel: NUM,
      minDisplayValue: NUM,
      maxDisplayValue: NUM,
      lastDisplayValue: NUM,
    });

    const numRows = queryRes.numRows();
    const timestampsRel = new Float32Array(numRows);
    const timestampsRelNext = new Float32Array(numRows);
    const minDisplayValues = new Float32Array(numRows);
    const maxDisplayValues = new Float32Array(numRows);
    const lastDisplayValues = new Float32Array(numRows);
    let min = 0;
    let max = 0;

    for (let row = 0; it.valid(); it.next(), row++) {
      if (signal.isCancelled) throw QUERY_CANCELLED;
      if (row % 50 === 0 && task.shouldYield()) {
        await task.yield();
      }

      timestampsRel[row] = it.tsRel;
      minDisplayValues[row] = it.minDisplayValue;
      maxDisplayValues[row] = it.maxDisplayValue;
      lastDisplayValues[row] = it.lastDisplayValue;
      min = Math.min(min, it.minDisplayValue);
      max = Math.max(max, it.maxDisplayValue);

      if (row > 0) {
        // Fill in the next
        timestampsRelNext[row - 1] = it.tsRel;
      }
    }

    if (numRows > 0) {
      timestampsRelNext[numRows - 1] = Number(end - start);
    }

    return {
      start,
      end,
      timestampsRel,
      timestampsRelNext,
      minDisplayValues,
      maxDisplayValues,
      lastDisplayValues,
      limits: {
        min,
        max,
      },
    };
  }

  protected get engine() {
    return this.trace.engine;
  }
}

// Returns a SQL expression that computes the display value from a table
// with `ts` and `value` columns, given the counter mode.
export function counterValueExpression(yMode: YMode): string {
  switch (yMode) {
    case 'value':
      return 'value';
    case 'delta':
      return 'lead(value, 1, value) over (order by ts) - value';
    case 'rate':
      return '(lead(value, 1, value) over (order by ts) - value) / ((lead(ts, 1, 100) over (order by ts) - ts) / 1e9)';
    default:
      assertUnreachable(yMode);
  }
}

// Returns the display label for a counter value given the mode.
export function counterDisplayLabel(yMode: YMode): string {
  switch (yMode) {
    case 'value':
      return 'Value';
    case 'delta':
      return 'Delta';
    case 'rate':
      return 'Rate';
    default:
      assertUnreachable(yMode);
  }
}

// Returns the unit string for a counter value given the mode.
export function counterDisplayUnit(
  yMode: YMode,
  unit: string,
  rateUnit: string,
): string {
  switch (yMode) {
    case 'value':
      return unit;
    case 'delta':
      return `\u0394${unit}`;
    case 'rate':
      return rateUnit;
    default:
      assertUnreachable(yMode);
  }
}

// Rounds n up to the next human-readable value.
function roundUp(n: number): number {
  if (n === 0) return 0;
  const exp = Math.ceil(Math.log10(Math.abs(n)));
  const step = Math.pow(10, exp) / 20;
  return Math.ceil(n / step) * step;
}

// Rounds n down to the previous human-readable value.
function roundDown(n: number): number {
  if (n === 0) return 0;
  const exp = Math.ceil(Math.log10(Math.abs(n)));
  const step = Math.pow(10, exp) / 20;
  return Math.floor(n / step) * step;
}
