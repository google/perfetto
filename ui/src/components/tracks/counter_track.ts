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
import {assertUnreachable, ensureDefined} from '../../base/assert';
import {searchSegment} from '../../base/binary_search';
import {HSLColor, hexToRgb, rgbToHsl} from '../../base/color';
import type {Point2D} from '../../base/geom';
import {formatNumber} from '../../base/number_format';
import {
  type CancellationSignal,
  TASK_CANCELLED,
  AsyncMemo,
  AtomicTaskQueue,
} from '../../base/async_memo';
import {Icons} from '../../base/semantic_icons';
import type {duration, time} from '../../base/time';
import type {TimeScale} from '../../base/time_scale';
import type {Trace} from '../../public/trace';
import type {
  TrackMouseEvent,
  TrackMouseWheelEvent,
  TrackRenderContext,
  TrackRenderer,
  TrackSetting,
  TrackSettingDescriptor,
} from '../../public/track';
import {NUM} from '../../trace_processor/query_result';
import {
  createPerfettoTable,
  makeTempName,
} from '../../trace_processor/sql_utils';
import {MenuDivider, MenuItem} from '../../widgets/menu';
import {checkerboardExcept} from '../checkerboard';
import {chartColorVar} from '../widgets/charts_svg/common';
import {ChartTooltip} from '../widgets/charts_svg/tooltip';
import {BufferedBounds} from './buffered_bounds';
import {RangeSharer} from './range_sharer';

export type ChartHeightSize = 1 | 2 | 4 | 8 | 16 | 32;
export type YMode = 'value' | 'delta' | 'rate';
export type YRange = 'all' | 'viewport';

// How a track carrying several counters draws them: overlaid on one axis,
// stacked on top of each other, or in a lane each.
export type CounterLayout = 'lines' | 'stacked' | 'lanes';

interface Limits {
  readonly min: number;
  readonly max: number;
}

// The mipmap tables of every series on the track, indexed like the series.
interface MipmapTables extends AsyncDisposable {
  readonly tables: ReadonlyArray<{
    readonly tableName: string; // The name of the virtual table containing the mipmap data which can be used in SQL queries.
    readonly globalLimits: Limits; // Min and max values across the entire trace.
  }>;
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

/** One counter plotted on a counter track. */
export interface CounterSeriesSpec {
  /** Shown in the legend and tooltip. */
  readonly name: string;

  /** SQL source selecting the necessary data (must expose ts and value). */
  readonly sqlSource: string;

  /** Unit string to display in tooltip and labels. */
  readonly unit?: string;

  /** What to display when yMode is 'rate'. */
  readonly rateUnit?: string;
}

interface GridLine {
  readonly value: number;
  readonly label: string;
}

interface Series {
  readonly spec: CounterSeriesSpec;
  readonly index: number;
  hidden: boolean;
  frame?: DataFrame;
  globalLimits?: Limits;
}

const BUCKETS_PER_PIXEL = 2;
const TRACK_HEIGHT_PX = 40;
const TRACK_PADDING = 2; // px gap between waveform peak/trough and track edge

const MIN_PLOT_HEIGHT_PX = 18;
const MAX_PLOT_HEIGHT_PX = 1000;

// SQLite caps a compound SELECT at 500 terms, so queries with a UNION ALL term
// per series are split into batches of this many series.
const SERIES_PER_QUERY = 500;
const LEGEND_HEIGHT_PX = 14;
const LEGEND_SWATCH_PX = 8;
const LEGEND_GAP_PX = 10;
const LEGEND_FADE_PX = 16;
const PLOT_PAD_TOP = 9;
const PLOT_PAD_BOTTOM = 5;
const TICK_SPACING_PX = 34;
const AXIS_FONT = '10px Roboto Condensed';
const AREA_ALPHA_TOP = 0.32;
const AREA_ALPHA_BOTTOM = 0.02;
const BAND_ALPHA = 0.18;
const DIM_ALPHA = 0.2;
// How near the cursor must be to a line to pick it out.
const FOCUS_TOLERANCE_PX = 8;
// Series have independent sample times, so the stacked layout resamples them
// onto columns this wide before summing.
const STACK_COLUMN_PX = 3;

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

const layoutDescriptor: TrackSettingDescriptor<CounterLayout> = {
  name: 'Layout',
  description: 'lines, stacked, lanes',
  render(setter, values) {
    const value = valueIfAllEqual(values);
    const option = (label: string, layout: CounterLayout) =>
      m(MenuItem, {
        label,
        onclick: () => setter(layout),
        icon: value === layout ? Icons.RadioChecked : Icons.RadioUnchecked,
      });
    return m(MenuItem, {label: `Layout (currently: ${value ?? 'mixed'})`}, [
      option('Lines', 'lines'),
      option('Stacked', 'stacked'),
      option('Lanes', 'lanes'),
    ]);
  },
};

export interface CounterTrackAttrs {
  /** The trace object used to run queries. */
  readonly trace: Trace;

  /** A unique, reproducible ID for this track. */
  readonly uri: string;

  /**
   * SQL source selecting the necessary data (must expose ts and value).
   * Required unless `series` is given.
   */
  readonly sqlSource?: string;

  /**
   * Several counters to plot on this one track, sharing its time axis. Takes
   * the place of `sqlSource`.
   */
  readonly series?: ReadonlyArray<CounterSeriesSpec>;

  /** How several series are drawn. Defaults to 'lines'. */
  readonly layout?: CounterLayout;

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

  /** Optional extra items for the bottom of the track's menu. */
  menuItems?(): m.Children;

  /** Optional lifecycle callback, called once before the first render. */
  onInit?(): Promise<void>;
}

export type CounterDisplaySettings = Pick<
  CounterTrackAttrs,
  'yMode' | 'yRange' | 'yDisplay' | 'yRangeRounding'
>;

export class CounterTrack implements TrackRenderer {
  // QuerySlot infrastructure
  private readonly queue = new AtomicTaskQueue();
  private readonly initSlot = new AsyncMemo<AsyncDisposable | void>(this.queue);
  private readonly tablesSlot = new AsyncMemo<MipmapTables>(this.queue);
  private readonly dataSlot = new AsyncMemo<ReadonlyArray<DataFrame>>(
    this.queue,
  );

  // Buffered bounds tracking
  private readonly bufferedBounds = new BufferedBounds();

  protected readonly trace: Trace;
  protected readonly uri: string;
  private readonly series: ReadonlyArray<Series>;
  private readonly rangeSharer: RangeSharer;
  private readonly onInitFn?: () => Promise<void>;
  private readonly menuItemsFn?: () => m.Children;

  // Mutable display settings (changed via the settings menu)
  protected yMode: YMode;
  private yRange: YRange;
  private yDisplay: 'zero' | 'minmax' | 'log';
  private yRangeRounding: 'strict' | 'human_readable';
  private chartHeightSize: ChartHeightSize;
  private layout: CounterLayout;

  // The height of one plot, once the track has been resized by dragging.
  private plotHeightPx?: number;

  // Immutable display options (set from attrs)
  private readonly _unit?: string;
  private readonly _rateUnit?: string;
  private readonly yOverrideMaximum?: number;
  private readonly yOverrideMinimum?: number;
  private readonly yRangeSharingKey?: string;

  // The hovered sample of each series, cached from the most recent mouse move
  // event.
  private hovers: ReadonlyArray<HoverState | undefined> = [];

  // The series under the cursor. The others are dimmed.
  private focused?: Series;

  // Where the last render put the plot and its axis, for mapping the mouse
  // back onto a series.
  private plot = {top: 0, height: 0, yMin: 0, yMax: 0};

  // Legend chips from the last render, in unscrolled coordinates.
  private legendChips: ReadonlyArray<{
    readonly series: Series;
    readonly left: number;
    readonly right: number;
  }> = [];
  private legendScrollPx = 0;
  private legendOverflowPx = 0;

  constructor(attrs: CounterTrackAttrs) {
    const {
      trace,
      uri,
      sqlSource,
      series,
      layout = 'lines',
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
    this.yMode = yMode;
    this.yRange = yRange;
    this.yDisplay = yDisplay;
    this.yRangeRounding = yRangeRounding;
    this.chartHeightSize = chartHeightSize;
    this.layout = layout;
    this._unit = unit;
    this._rateUnit = rateUnit;
    this.yOverrideMaximum = yOverrideMaximum;
    this.yOverrideMinimum = yOverrideMinimum;
    this.yRangeSharingKey = yRangeSharingKey;
    this.rangeSharer = RangeSharer.getRangeSharer(trace);
    this.onInitFn = onInit;
    this.menuItemsFn = attrs.menuItems?.bind(attrs);

    const specs = series ?? [
      {name: uri, sqlSource: sqlSource ?? '', unit, rateUnit},
    ];
    this.series = specs.map((spec, index) => ({spec, index, hidden: false}));
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
      as: ensureDefined(attrs.sqlSource),
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

  /** The current display settings, in the form CounterTrackAttrs takes. */
  get displaySettings(): CounterDisplaySettings {
    const {yMode, yRange, yDisplay, yRangeRounding} = this;
    return {yMode, yRange, yDisplay, yRangeRounding};
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
          this.plotHeightPx = undefined;
        },
      }),
      ...(this.isMultiSeries
        ? [
            setting({
              descriptor: layoutDescriptor,
              value: this.layout,
              update: (value) => {
                this.layout = value;
              },
            }),
          ]
        : []),
    ];
  }

  getTrackMenuItems(): m.Children {
    return [
      this.isMultiSeries && [
        m(MenuDivider),
        m(
          MenuItem,
          {label: 'Counters', icon: 'legend_toggle'},
          m(MenuItem, {
            label: 'Show all',
            icon: 'visibility',
            disabled: this.series.every((s) => !s.hidden),
            closePopupOnClick: false,
            onclick: () => this.series.forEach((s) => this.setHidden(s, false)),
          }),
          m(MenuItem, {
            label: 'Hide all',
            icon: Icons.Hide,
            disabled: this.series.every((s) => s.hidden),
            closePopupOnClick: false,
            onclick: () => this.series.forEach((s) => this.setHidden(s, true)),
          }),
          m(MenuDivider),
          this.series.map((s) =>
            m(MenuItem, {
              label: s.spec.name,
              title: s.spec.name,
              icon: s.hidden ? Icons.BlankCheckbox : Icons.Checkbox,
              closePopupOnClick: false,
              onclick: () => this.setHidden(s, !s.hidden),
            }),
          ),
        ),
      ],
      this.menuItemsFn?.(),
    ];
  }

  // Returns the height of the track in pixels.
  getHeight() {
    const plotHeight =
      this.plotHeightPx ?? TRACK_HEIGHT_PX * this.chartHeightSize;
    return Math.round(this.legendHeight + plotHeight * this.plotCount);
  }

  setHeight(heightPx: number) {
    const plotHeight = (heightPx - this.legendHeight) / this.plotCount;
    this.plotHeightPx = Math.min(
      MAX_PLOT_HEIGHT_PX,
      Math.max(MIN_PLOT_HEIGHT_PX, plotHeight),
    );
  }

  // Called every render cycle to draw the track to the timeline.
  render(trackCtx: TrackRenderContext): void {
    const loaded = this.useData(trackCtx);

    if (this.isMultiSeries) {
      this.renderMultiSeries(trackCtx, loaded);
      return;
    }

    const series = this.series[0];
    const data = series.frame;
    if (!loaded || data === undefined) {
      return; // No data ready yet
    }

    const {ctx, size, timescale, colors, renderer} = trackCtx;

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
    const {yMin, yMax} = this.computeYRange(
      series.globalLimits ?? displayValueRange,
      displayValueRange,
    );

    const trackHeight = size.height;
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

    const hover = this.hovers[0];
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
      this.drawLabel(
        ctx,
        colors,
        this.formatYValue(series.spec, yMax),
        0,
        0,
        'top',
      );
    }

    // Draw the min label as long as it's not 0
    if (yMin !== 0) {
      this.drawLabel(
        ctx,
        colors,
        this.formatYValue(series.spec, yMin),
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
        ctx.fillRect(counterEndPx, 0, endPx - counterEndPx, trackHeight);
      }
    }

    this.drawLoading(trackCtx);
  }

  onMouseMove({x, y, timescale}: TrackMouseEvent) {
    const hovers = this.series.map((s) =>
      s.hidden ? undefined : findSampleAtPos(s.frame, {x, y}, timescale),
    );
    const focused = this.seriesAt(x, y, hovers);
    if (
      focused !== this.focused ||
      JSON.stringify(hovers) !== JSON.stringify(this.hovers)
    ) {
      m.redraw();
    }
    this.hovers = hovers;
    this.focused = focused;
  }

  onMouseOut() {
    if (this.hovers.some((h) => h !== undefined)) {
      m.redraw();
    }
    this.hovers = [];
    this.focused = undefined;
  }

  onMouseClick({x, y}: TrackMouseEvent): boolean {
    const chip = this.legendChipAt(x, y);
    if (chip === undefined) return false;
    this.setHidden(chip.series, !chip.series.hidden);
    return true;
  }

  onMouseWheel({y, deltaX, deltaY, shiftKey}: TrackMouseWheelEvent): boolean {
    if (y >= this.legendHeight || this.legendOverflowPx === 0) return false;
    const delta =
      Math.abs(deltaX) > Math.abs(deltaY) ? deltaX : shiftKey ? deltaY : 0;
    const scroll = Math.min(
      this.legendOverflowPx,
      Math.max(0, this.legendScrollPx + delta),
    );
    if (scroll === this.legendScrollPx) return false;
    this.legendScrollPx = scroll;
    return true;
  }

  renderTooltip(): m.Children {
    if (!this.isMultiSeries) {
      const hover = this.hovers[0];
      if (!hover) return undefined;
      const text = this.formatYValue(
        this.series[0].spec,
        hover.lastDisplayValue,
        (v, unit) => `${v.toLocaleString()}${unit}`,
      );
      return m('.pf-track__tooltip', text);
    }

    const visible = this.visibleSeries();
    const hovered = visible.filter((s) => this.hovers[s.index] !== undefined);
    if (hovered.length === 0) return undefined;

    const focused = this.focused;
    const focusedHover = focused && this.hovers[focused.index];
    let total = 0;
    for (const s of hovered) {
      total += this.hovers[s.index]?.lastDisplayValue ?? 0;
    }

    return [
      focused &&
        focusedHover &&
        m(ChartTooltip.Row, {
          name: focused.spec.name,
          value: this.formatYValue(focused.spec, focusedHover.lastDisplayValue),
          swatch: chartColorVar(focused.index),
        }),
      // Summing is only meaningful for values in one unit on a linear scale.
      !this.isLog &&
        this.hasSharedUnit(visible) &&
        m(ChartTooltip.Row, {
          name: 'Total',
          value: this.formatYValue(visible[0].spec, total),
          tweak: focused && 'muted',
        }),
    ];
  }

  // -- Private methods --

  private get isMultiSeries(): boolean {
    return this.series.length > 1;
  }

  private get legendHeight(): number {
    return this.isMultiSeries ? LEGEND_HEIGHT_PX : 0;
  }

  // The number of plots stacked vertically: one per shown counter in lanes.
  private get plotCount(): number {
    if (this.isMultiSeries && this.layout === 'lanes') {
      return Math.max(1, this.visibleSeries().length);
    }
    return 1;
  }

  private visibleSeries(): Series[] {
    return this.series.filter((s) => !s.hidden);
  }

  private setHidden(series: Series, hidden: boolean) {
    if (series.hidden === hidden) return;
    series.hidden = hidden;
    this.hovers = [];
    this.focused = undefined;
  }

  private hasSharedUnit(series: ReadonlyArray<Series>): boolean {
    return new Set(series.map((s) => s.spec.unit)).size === 1;
  }

  // Stacking sums the values, which is meaningless for their logs.
  private get isLog(): boolean {
    return this.yDisplay === 'log' && this.layout !== 'stacked';
  }

  private seriesColor(series: Series, colors: ReadonlyArray<string>) {
    const hex = colors[series.index % colors.length];
    return new HSLColor(rgbToHsl(hexToRgb(hex)));
  }

  private alphaOf(series: Series): number {
    return this.focused === undefined || this.focused === series
      ? 1
      : DIM_ALPHA;
  }

  // The series the cursor picks out: the chip it is over in the legend, and
  // otherwise the line it is on, the band it is in, or the lane it is in.
  private seriesAt(
    x: number,
    y: number,
    hovers: ReadonlyArray<HoverState | undefined>,
  ): Series | undefined {
    if (!this.isMultiSeries) return undefined;
    if (y < this.legendHeight) {
      const series = this.legendChipAt(x, y)?.series;
      return series?.hidden ? undefined : series;
    }

    const visible = this.visibleSeries().filter(
      (s) => hovers[s.index] !== undefined,
    );
    const {top, height, yMin, yMax} = this.plot;
    const valueOf = (s: Series) => hovers[s.index]?.lastDisplayValue ?? 0;
    switch (this.layout) {
      case 'lines': {
        let best: Series | undefined;
        let bestDist = FOCUS_TOLERANCE_PX;
        for (const s of visible) {
          const dist = Math.abs(
            valueToY(valueOf(s), yMin, yMax, top, height) - y,
          );
          if (dist <= bestDist) {
            best = s;
            bestDist = dist;
          }
        }
        return best;
      }
      case 'stacked': {
        let base = 0;
        for (const s of visible) {
          const next = base + Math.max(0, valueOf(s));
          const bandTop = valueToY(next, yMin, yMax, top, height);
          const bandBottom = valueToY(base, yMin, yMax, top, height);
          if (y >= bandTop && y <= bandBottom) return s;
          base = next;
        }
        return undefined;
      }
      case 'lanes': {
        const shown = this.visibleSeries();
        const lane = Math.floor(((y - top) / height) * shown.length);
        return shown[lane];
      }
      default:
        assertUnreachable(this.layout);
    }
  }

  private legendChipAt(x: number, y: number) {
    if (y >= this.legendHeight) return undefined;
    const contentX = x + this.legendScrollPx;
    return this.legendChips.find(
      (c) => contentX >= c.left && contentX <= c.right,
    );
  }

  private renderMultiSeries(trackCtx: TrackRenderContext, loaded: boolean) {
    const {size} = trackCtx;
    this.drawLegend(trackCtx);

    const visible = this.visibleSeries();
    if (!loaded || visible.length === 0) return;

    const top = this.legendHeight;
    const height = size.height - top;
    switch (this.layout) {
      case 'lines':
        this.drawLines(trackCtx, visible, top, height);
        break;
      case 'stacked':
        this.drawStacked(trackCtx, visible, top, height);
        break;
      case 'lanes': {
        this.plot = {top, height, yMin: 0, yMax: 0};
        const laneHeight = height / visible.length;
        visible.forEach((series, i) => {
          this.drawLane(trackCtx, series, top + i * laneHeight, laneHeight);
        });
        break;
      }
      default:
        assertUnreachable(this.layout);
    }

    this.drawLoading(trackCtx);
  }

  // Every series on one axis spanning all of them.
  private drawLines(
    trackCtx: TrackRenderContext,
    visible: ReadonlyArray<Series>,
    top: number,
    height: number,
  ) {
    const {yMin, yMax} = this.computeYRange(
      unionLimits(visible.map((s) => s.globalLimits)),
      unionLimits(visible.map((s) => s.frame?.limits)),
    );
    this.plot = {top, height, yMin, yMax};
    const yOf = (value: number) => valueToY(value, yMin, yMax, top, height);

    const grid = this.gridFor(visible, height, yMin, yMax);
    this.drawGridLines(trackCtx, grid, yOf);
    // The focused line goes last, so nothing is drawn over it.
    const focused = this.focused;
    const ordered = focused
      ? [...visible.filter((s) => s !== focused), focused]
      : visible;
    for (const series of ordered) {
      this.strokeSeries(trackCtx, series, yOf, top, height, false);
    }
    this.drawGridLabels(trackCtx, grid, yOf);
  }

  // Each series stacked on those before it, so the top edge is their total.
  private drawStacked(
    trackCtx: TrackRenderContext,
    visible: ReadonlyArray<Series>,
    top: number,
    height: number,
  ) {
    const {ctx, size, timescale, colors} = trackCtx;

    const columnCount = Math.ceil(size.width / STACK_COLUMN_PX) + 1;
    const columns = new Float32Array(columnCount);
    for (let c = 0; c < columnCount; c++) {
      columns[c] = Math.min(size.width, c * STACK_COLUMN_PX);
    }
    const values = visible.map((series) => {
      const column = new Float32Array(columnCount);
      for (let c = 0; c < columnCount; c++) {
        column[c] = Math.max(0, valueAtX(series.frame, columns[c], timescale));
      }
      return column;
    });

    // The axis tops out at the most the stack can reach, over the whole trace
    // or, when zooming on scroll, over the columns on screen.
    let total = 0;
    if (this.yRange === 'viewport') {
      for (let c = 0; c < columnCount; c++) {
        let sum = 0;
        for (const column of values) sum += column[c];
        total = Math.max(total, sum);
      }
    } else {
      for (const series of visible) {
        total += Math.max(0, series.globalLimits?.max ?? 0);
      }
    }
    const yMax =
      this.yRangeRounding === 'human_readable' ? roundUp(total) : total;
    this.plot = {top, height, yMin: 0, yMax};
    const yOf = (value: number) => valueToY(value, 0, yMax, top, height);

    const grid = this.gridFor(visible, height, 0, yMax);
    this.drawGridLines(trackCtx, grid, yOf);

    ctx.save();
    ctx.beginPath();
    ctx.rect(0, top, size.width, height);
    ctx.clip();
    ctx.lineWidth = 1.2;
    const base = new Float32Array(columnCount);
    visible.forEach((series, i) => {
      const color = this.seriesColor(series, colors.CHART_COLORS);
      const column = values[i];

      ctx.beginPath();
      for (let c = 0; c < columnCount; c++) {
        ctx.lineTo(columns[c], yOf(base[c] + column[c]));
      }
      for (let c = columnCount - 1; c >= 0; c--) {
        ctx.lineTo(columns[c], yOf(base[c]));
      }
      ctx.closePath();
      const gradient = ctx.createLinearGradient(0, top, 0, top + height);
      gradient.addColorStop(0, color.setAlpha(0.7).cssString);
      gradient.addColorStop(1, color.setAlpha(0.25).cssString);
      ctx.globalAlpha = this.alphaOf(series);
      ctx.fillStyle = gradient;
      ctx.fill();

      ctx.beginPath();
      for (let c = 0; c < columnCount; c++) {
        base[c] += column[c];
        ctx.lineTo(columns[c], yOf(base[c]));
      }
      ctx.strokeStyle = color.cssString;
      ctx.stroke();
    });
    ctx.restore();

    this.drawGridLabels(trackCtx, grid, yOf);
  }

  // A series in a band of the track, with its own axis and name.
  private drawLane(
    trackCtx: TrackRenderContext,
    series: Series,
    top: number,
    height: number,
  ) {
    const {ctx, size, colors} = trackCtx;
    const limits = series.frame?.limits ?? {min: 0, max: 0};
    const {yMin, yMax} = this.computeYRange(
      series.globalLimits ?? limits,
      limits,
    );
    const yOf = (value: number) => valueToY(value, yMin, yMax, top, height);

    const bottom = Math.round(top + height) - 0.5;
    ctx.strokeStyle = colors.COLOR_BORDER_SECONDARY;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, bottom);
    ctx.lineTo(size.width, bottom);
    ctx.stroke();

    const grid = this.gridFor([series], height, yMin, yMax);
    this.drawGridLines(trackCtx, grid, yOf);
    this.strokeSeries(trackCtx, series, yOf, top, height, true);
    this.drawGridLabels(trackCtx, grid, yOf);

    const name = series.spec.name;
    ctx.font = AXIS_FONT;
    const nameWidth = ctx.measureText(name).width;
    ctx.globalAlpha = 0.85;
    ctx.fillStyle = colors.COLOR_BACKGROUND;
    ctx.fillRect(size.width - nameWidth - 10, top + 1, nameWidth + 8, 12);
    ctx.globalAlpha = 1;
    ctx.textAlign = 'right';
    ctx.textBaseline = 'top';
    ctx.fillStyle = colors.COLOR_TEXT_MUTED;
    ctx.fillText(name, size.width - 6, top + 3);
  }

  // A series as a stepped line over the band of values each bucket covers,
  // optionally with an area fading down to the bottom of the plot.
  private strokeSeries(
    trackCtx: TrackRenderContext,
    series: Series,
    yOf: (value: number) => number,
    top: number,
    height: number,
    area: boolean,
  ) {
    const {ctx, size, timescale, colors} = trackCtx;
    const data = series.frame;
    if (data === undefined || data.timestampsRel.length === 0) return;

    const color = this.seriesColor(series, colors.CHART_COLORS);
    const alpha = this.alphaOf(series);
    const count = data.timestampsRel.length;
    const pxPerNs = timescale.durationToPx(1n);
    const baseOffsetPx = timescale.timeToPx(data.start);
    const xOf = (i: number) => data.timestampsRel[i] * pxPerNs + baseOffsetPx;
    const xNextOf = (i: number) =>
      data.timestampsRelNext[i] * pxPerNs + baseOffsetPx;

    ctx.save();
    ctx.beginPath();
    ctx.rect(0, top, size.width, height);
    ctx.clip();

    if (area) {
      const bottom = top + height;
      ctx.beginPath();
      ctx.moveTo(xOf(0), bottom);
      for (let i = 0; i < count; i++) {
        const y = yOf(data.lastDisplayValues[i]);
        ctx.lineTo(xOf(i), y);
        ctx.lineTo(xNextOf(i), y);
      }
      ctx.lineTo(xNextOf(count - 1), bottom);
      ctx.closePath();
      const gradient = ctx.createLinearGradient(0, top, 0, bottom);
      gradient.addColorStop(0, color.setAlpha(AREA_ALPHA_TOP).cssString);
      gradient.addColorStop(1, color.setAlpha(AREA_ALPHA_BOTTOM).cssString);
      ctx.globalAlpha = alpha;
      ctx.fillStyle = gradient;
      ctx.fill();
    }

    ctx.beginPath();
    for (let i = 0; i < count; i++) {
      const x = xOf(i);
      const yHi = yOf(data.maxDisplayValues[i]);
      const yLo = yOf(data.minDisplayValues[i]);
      ctx.rect(x, yHi, Math.max(1, xNextOf(i) - x), yLo - yHi);
    }
    ctx.globalAlpha = BAND_ALPHA * alpha;
    ctx.fillStyle = color.cssString;
    ctx.fill();

    ctx.beginPath();
    for (let i = 0; i < count; i++) {
      const y = yOf(data.lastDisplayValues[i]);
      ctx.lineTo(xOf(i), y);
      ctx.lineTo(xNextOf(i), y);
    }
    ctx.globalAlpha = alpha;
    ctx.strokeStyle = color.cssString;
    ctx.lineWidth = 1.5;
    ctx.stroke();
    ctx.restore();
  }

  // Values to draw gridlines at, one every TICK_SPACING_PX or so. A plot too
  // short for two just marks its top. Labels only carry a unit that all of
  // the plotted series share.
  private gridFor(
    series: ReadonlyArray<Series>,
    height: number,
    yMin: number,
    yMax: number,
  ): ReadonlyArray<GridLine> {
    if (!(yMax > yMin)) return [];
    const count = Math.floor(height / TICK_SPACING_PX);
    const values =
      count < 2
        ? [yMax]
        : niceTicks(yMin, yMax, count).filter((v) => v >= yMin && v <= yMax);
    const units = this.hasSharedUnit(series) ? series[0].spec : {};
    return values.map((value) => ({
      value,
      label: this.formatYValue(units, value),
    }));
  }

  private drawGridLines(
    {ctx, size, colors}: TrackRenderContext,
    grid: ReadonlyArray<GridLine>,
    yOf: (value: number) => number,
  ) {
    ctx.font = AXIS_FONT;
    ctx.strokeStyle = colors.COLOR_BORDER_SECONDARY;
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (const {value, label} of grid) {
      const y = Math.round(yOf(value)) + 0.5;
      ctx.moveTo(ctx.measureText(label).width + 10, y);
      ctx.lineTo(size.width, y);
    }
    ctx.stroke();
  }

  // Drawn after the data, on a backing, so no line strikes through them.
  private drawGridLabels(
    {ctx, colors}: TrackRenderContext,
    grid: ReadonlyArray<GridLine>,
    yOf: (value: number) => number,
  ) {
    ctx.font = AXIS_FONT;
    ctx.textAlign = 'left';
    ctx.textBaseline = 'middle';
    for (const {value, label} of grid) {
      const y = Math.round(yOf(value)) + 0.5;
      ctx.globalAlpha = 0.85;
      ctx.fillStyle = colors.COLOR_BACKGROUND;
      ctx.fillRect(0, y - 6, ctx.measureText(label).width + 8, 12);
      ctx.globalAlpha = 1;
      ctx.fillStyle = colors.COLOR_TEXT_MUTED;
      ctx.fillText(label, 4, y);
    }
  }

  // A chip per series, struck through when hidden. Scrolls sideways when it
  // doesn't fit, fading out at the edges with more to show.
  private drawLegend({ctx, size, colors}: TrackRenderContext) {
    ctx.font = AXIS_FONT;
    ctx.textAlign = 'left';
    ctx.textBaseline = 'middle';
    const midY = LEGEND_HEIGHT_PX / 2;

    const chips: {series: Series; left: number; right: number}[] = [];
    let left = 4;
    for (const series of this.series) {
      const right =
        left + LEGEND_SWATCH_PX + 4 + ctx.measureText(series.spec.name).width;
      chips.push({series, left, right});
      left = right + LEGEND_GAP_PX;
    }
    this.legendChips = chips;
    this.legendOverflowPx = Math.max(0, left - LEGEND_GAP_PX + 4 - size.width);
    this.legendScrollPx = Math.min(this.legendScrollPx, this.legendOverflowPx);

    const scroll = this.legendScrollPx;
    for (const {series, left, right} of chips) {
      const x = left - scroll;
      if (right - scroll < 0 || x > size.width) continue;
      const textX = x + LEGEND_SWATCH_PX + 4;
      ctx.globalAlpha = series.hidden ? 0.35 : 1;
      ctx.fillStyle =
        colors.CHART_COLORS[series.index % colors.CHART_COLORS.length];
      ctx.fillRect(
        x,
        midY - LEGEND_SWATCH_PX / 2,
        LEGEND_SWATCH_PX,
        LEGEND_SWATCH_PX,
      );
      ctx.fillStyle = colors.COLOR_TEXT;
      ctx.fillText(series.spec.name, textX, midY);
      if (series.hidden) {
        ctx.strokeStyle = colors.COLOR_TEXT;
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(textX, midY);
        ctx.lineTo(right - scroll, midY);
        ctx.stroke();
      }
    }

    ctx.fillStyle = colors.COLOR_BACKGROUND;
    for (let i = 0; i < LEGEND_FADE_PX; i++) {
      ctx.globalAlpha = 1 - i / LEGEND_FADE_PX;
      if (scroll > 0) {
        ctx.fillRect(i, 0, 1, LEGEND_HEIGHT_PX);
      }
      if (scroll < this.legendOverflowPx) {
        ctx.fillRect(size.width - 1 - i, 0, 1, LEGEND_HEIGHT_PX);
      }
    }
    ctx.globalAlpha = 1;
  }

  // If the cached data doesn't fully cover the visible time range, show a
  // gray rectangle with a "Loading..." label.
  private drawLoading({ctx, size, timescale}: TrackRenderContext) {
    const loadedBounds = this.bufferedBounds.bounds;
    checkerboardExcept(
      ctx,
      size.height,
      0,
      size.width,
      timescale.timeToPx(loadedBounds.start),
      timescale.timeToPx(loadedBounds.end),
    );
  }

  // Whether the samples for the current window are loaded.
  private useData(trackCtx: TrackRenderContext): boolean {
    const {size, visibleWindow} = trackCtx;

    // Step 0: Call onInit with a constant key
    const initResult = this.initSlot.use({
      key: {init: true},
      compute: () => this.onInitFn?.() ?? Promise.resolve(),
    });

    if (initResult.isPending) {
      return false;
    }

    // Step 1: Get the mipmap tables (created once per SQL source + options)
    const {yMode, isLog} = this;
    const tablesResult = this.tablesSlot.use({
      key: {
        sqlSources: this.series.map((s) => s.spec.sqlSource),
        yMode,
        isLog,
      },
      compute: () => this.createMipmapTables(isLog),
    });

    const tables = tablesResult.data?.tables;
    if (tables === undefined) return false;

    for (const series of this.series) {
      series.globalLimits = tables[series.index].globalLimits;
    }

    // Step 2: Calculate buffered bounds and fetch counter data
    const visibleSpan = visibleWindow.toTimeSpan();
    const windowSizePx = Math.max(1, size.width);
    const bucketSize = this.computeBucketSize(
      visibleSpan.duration,
      windowSizePx,
    );
    const bounds = this.bufferedBounds.update(visibleSpan, bucketSize);

    // Step 3: Fetch counter data using QuerySlot. Hidden series are fetched
    // too, so showing one again is instant.
    const queryStart = bounds.start;
    const queryEnd = bounds.end;
    const tableNames = tables.map((table) => table.tableName);
    const dataResult = this.dataSlot.use({
      key: {
        start: bounds.start,
        end: bounds.end,
        resolution: bounds.resolution,
        tableNames,
      },
      compute: async (signal) => {
        return await this.trace.taskTracker.track(
          this.fetchCounterData(
            tableNames,
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

    const frames = dataResult.data;
    if (frames === undefined) return false;
    this.series.forEach((series, i) => (series.frame = frames[i]));
    return true;
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
    {unit = '', rateUnit}: {unit?: string; rateUnit?: string},
    value: number,
    fmt: (v: number, unit: string) => string = formatNumber,
  ): string {
    const {yMode} = this;
    const v = this.isLog ? Math.exp(value) : value;

    switch (yMode) {
      case 'value':
        return fmt(v, unit);
      case 'delta':
        return `\u0394${fmt(v, unit)}`;
      case 'rate':
        return fmt(v, rateUnit ?? `\u0394${unit}/s`);
      default:
        assertUnreachable(yMode);
    }
  }

  // The underlying table has `ts` and `value` columns.
  private getValueExpression(log: boolean): string {
    const valueExpr = counterValueExpression(this.yMode);

    if (log) {
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

  // Creates a mipmap table per series and reads their global limits, in one
  // query plus one per batch of series.
  private async createMipmapTables(log: boolean): Promise<MipmapTables> {
    const tableNames = this.series.map(() => makeTempName());
    await this.engine.query(
      this.series
        .map(
          ({spec}, i) => `
            CREATE VIRTUAL TABLE ${tableNames[i]}
            USING __intrinsic_counter_mipmap((
              SELECT
                ts,
                ${this.getValueExpression(log)} AS value
              FROM (${spec.sqlSource})
            ));`,
        )
        .join('\n'),
    );

    // Fetch the global limits. A series with no samples has no row, and keeps
    // empty limits.
    const globalLimits = tableNames.map(() => ({min: 0, max: 0}));
    for (const [offset, batch] of batches(tableNames)) {
      const limitsQuery = await this.engine.query(
        batch
          .map(
            (tableName, i) => `
              SELECT
                ${offset + i} AS seriesIndex,
                min_value AS min,
                max_value AS max
              FROM ${tableName}(
                trace_start(), trace_end() + 1, trace_dur() + 1
              )`,
          )
          .join(' UNION ALL '),
      );
      const it = limitsQuery.iter({seriesIndex: NUM, min: NUM, max: NUM});
      for (; it.valid(); it.next()) {
        globalLimits[it.seriesIndex] = {min: it.min, max: it.max};
      }
    }

    return {
      tables: tableNames.map((tableName, i) => ({
        tableName,
        globalLimits: globalLimits[i],
      })),
      [Symbol.asyncDispose]: async () => {
        await this.engine.query(
          tableNames.map((name) => `DROP TABLE IF EXISTS ${name};`).join('\n'),
        );
      },
    };
  }

  // Fetches every table's samples for the given bounds, in one query per batch
  // of tables - called from QuerySlot.
  private async fetchCounterData(
    tableNames: ReadonlyArray<string>,
    start: time,
    end: time,
    resolution: duration,
    signal: CancellationSignal,
  ): Promise<ReadonlyArray<DataFrame>> {
    const frames: DataFrame[] = [];
    for (const [, batch] of batches(tableNames)) {
      const queryRes = await this.engine.query(`
        SELECT
          table_index AS tableIndex,
          min_value AS minDisplayValue,
          max_value AS maxDisplayValue,
          MAX(0, MIN(last_ts - ${start}, ${end} - ${start})) AS tsRel,
          last_value AS lastDisplayValue
        FROM (
          ${batch
            .map(
              (tableName, i) => `
                SELECT ${i} AS table_index, *
                FROM ${tableName}(${start}, ${end}, ${resolution})`,
            )
            .join(' UNION ALL ')}
        )
        ORDER BY table_index, last_ts;
      `);

      if (signal.isCancelled) throw TASK_CANCELLED;

      const cols = queryRes.decodeColumns({
        tableIndex: NUM,
        tsRel: NUM,
        minDisplayValue: NUM,
        maxDisplayValue: NUM,
        lastDisplayValue: NUM,
      });

      // Rows are grouped by table and in time order within each.
      const numRows = queryRes.numRows();
      let row = 0;
      for (let i = 0; i < batch.length; i++) {
        const from = row;
        while (row < numRows && cols.tableIndex[row] === i) row++;
        frames.push(buildDataFrame(cols, from, row, start, end));
      }
    }
    return frames;
  }

  protected get engine() {
    return this.trace.engine;
  }

  protected get sqlSource(): string {
    return this.series[0].spec.sqlSource;
  }
}

// Splits items into batches of SERIES_PER_QUERY, each with its offset.
function batches<T>(items: ReadonlyArray<T>): Array<[number, T[]]> {
  const result: Array<[number, T[]]> = [];
  for (let i = 0; i < items.length; i += SERIES_PER_QUERY) {
    result.push([i, items.slice(i, i + SERIES_PER_QUERY)]);
  }
  return result;
}

// Slices the rows [from, to) out of the decoded columns into a dataframe.
function buildDataFrame(
  cols: {
    readonly tsRel: ArrayLike<number>;
    readonly minDisplayValue: ArrayLike<number>;
    readonly maxDisplayValue: ArrayLike<number>;
    readonly lastDisplayValue: ArrayLike<number>;
  },
  from: number,
  to: number,
  start: time,
  end: time,
): DataFrame {
  const numRows = to - from;
  const timestampsRel = new Float32Array(numRows);
  const minDisplayValues = new Float32Array(numRows);
  const maxDisplayValues = new Float32Array(numRows);
  const lastDisplayValues = new Float32Array(numRows);

  let min = 0;
  let max = 0;
  for (let i = 0; i < numRows; i++) {
    const row = from + i;
    timestampsRel[i] = cols.tsRel[row];
    minDisplayValues[i] = cols.minDisplayValue[row];
    maxDisplayValues[i] = cols.maxDisplayValue[row];
    lastDisplayValues[i] = cols.lastDisplayValue[row];
    min = Math.min(min, minDisplayValues[i]);
    max = Math.max(max, maxDisplayValues[i]);
  }

  // timestampsRelNext[i] = timestampsRel[i+1], last element = end - start.
  const timestampsRelNext = new Float32Array(numRows);
  if (numRows > 1) {
    timestampsRelNext.set(timestampsRel.subarray(1));
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

function findSampleAtPos(
  data: DataFrame | undefined,
  pos: Point2D,
  timescale: TimeScale,
): HoverState | undefined {
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

// The value a series holds at pixel x, or 0 before its first sample.
function valueAtX(
  data: DataFrame | undefined,
  x: number,
  timescale: TimeScale,
): number {
  if (data === undefined) return 0;
  const relNs = Number(timescale.pxToHpTime(x).toTime() - data.start);
  const [left] = searchSegment(data.timestampsRel, relNs);
  return left === -1 ? 0 : data.lastDisplayValues[left];
}

// Maps a value onto a plot occupying [top, top + height).
function valueToY(
  value: number,
  yMin: number,
  yMax: number,
  top: number,
  height: number,
): number {
  const drawHeight = Math.max(1, height - PLOT_PAD_TOP - PLOT_PAD_BOTTOM);
  const fraction = (value - yMin) / (yMax - yMin || 1);
  return top + PLOT_PAD_TOP + drawHeight * (1 - fraction);
}

function unionLimits(limits: ReadonlyArray<Limits | undefined>): Limits {
  let min = Infinity;
  let max = -Infinity;
  for (const l of limits) {
    if (l === undefined) continue;
    min = Math.min(min, l.min);
    max = Math.max(max, l.max);
  }
  return min <= max ? {min, max} : {min: 0, max: 0};
}

// Round values roughly `count` apart spanning [min, max].
function niceTicks(min: number, max: number, count: number): number[] {
  if (!(max > min)) return [];
  const rough = (max - min) / count;
  const magnitude = Math.pow(10, Math.floor(Math.log10(rough)));
  const norm = rough / magnitude;
  const step = (norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10) * magnitude;
  const ticks: number[] = [];
  for (let i = Math.ceil(min / step); i * step <= max; i++) {
    ticks.push(i * step);
  }
  return ticks;
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
