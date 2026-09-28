// Copyright (C) 2026 The Android Open Source Project
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
import {
  type CancellationSignal,
  TASK_CANCELLED,
  AsyncMemo,
  AtomicTaskQueue,
} from '../../base/async_memo';
import {searchSegment} from '../../base/binary_search';
import {HSLColor} from '../../base/color';
import {formatNumber} from '../../base/number_format';
import {Icons} from '../../base/semantic_icons';
import {cropText} from '../../base/string_utils';
import type {duration, time} from '../../base/time';
import type {TimeScale} from '../../base/time_scale';
import {checkerboardExcept} from '../../components/checkerboard';
import {BufferedBounds} from '../../components/tracks/buffered_bounds';
import {
  counterValueExpression,
  type YMode,
  type YRange,
} from '../../components/tracks/counter_track';
import {getChartThemeColors} from '../../components/widgets/charts/chart_theme';
import {ChartTooltip} from '../../components/widgets/charts_svg/tooltip';
import type {Trace} from '../../public/trace';
import type {
  TrackMouseEvent,
  TrackRenderContext,
  TrackRenderer,
  TrackSetting,
  TrackSettingDescriptor,
} from '../../public/track';
import {NUM} from '../../trace_processor/query_result';
import {Button} from '../../widgets/button';
import {MenuDivider, MenuItem, PopupMenu} from '../../widgets/menu';

/** A counter plotted on a timeseries track. */
export interface TimeseriesCounter {
  readonly id: number; // counter_track.id
  readonly name: string; // Shown in the legend and tooltip.
  readonly unit?: string; // Used to format its values.
}

export interface TimeseriesRendererOpts {
  /** The mode (value, delta or rate) to open in. Defaults to 'value'. */
  readonly yMode?: YMode;

  /** Extra items for the bottom of the track's layout & counters menu. */
  readonly extraMenuItems?: () => m.Children;
}

/**
 * Creates a renderer plotting several counters against one time axis: as
 * lines on one axis, as stacked areas, or (when expanded) a row per counter.
 */
export function createTimeseriesRenderer(
  trace: Trace,
  _uri: string,
  counters: ReadonlyArray<TimeseriesCounter>,
  opts: TimeseriesRendererOpts = {},
): TrackRenderer {
  return new TimeseriesRenderer(trace, counters, opts);
}

// How the counters share one plot: overlaid on one axis, or stacked on top of
// each other. Expanding the track instead gives each counter its own row.
type Layout = 'lines' | 'stacked';
type YDisplay = 'zero' | 'minmax' | 'log';
type YRounding = 'strict' | 'human_readable';
type ChartHeight = 1 | 2 | 4 | 8;

interface Limits {
  readonly min: number;
  readonly max: number;
}

// The mipmap table of every counter, indexed like the counters.
interface MipmapTables extends AsyncDisposable {
  readonly tableNames: ReadonlyArray<string>;
  readonly globalLimits: ReadonlyArray<Limits>; // Over the whole trace.
}

interface DataFrame {
  readonly start: time;
  readonly end: time;
  readonly limits: Limits; // Min and max values within the frame.
  readonly timestampsRel: Float32Array; // Relative to start.
  readonly timestampsRelNext: Float32Array; // Of the next sample.
  readonly minDisplayValues: Float32Array; // Min value within each bucket.
  readonly maxDisplayValues: Float32Array; // Max value within each bucket.
  readonly lastDisplayValues: Float32Array; // Final value within each bucket.
}

interface HoverState {
  readonly value: number;
  readonly tsRel: number;
}

interface Series {
  readonly counter: TimeseriesCounter;
  readonly index: number;
  color: HSLColor; // From the theme's chart palette.
  hidden: boolean;
  frame?: DataFrame;
  globalLimits?: Limits;
}

interface GridLine {
  readonly value: number;
  readonly label: string;
}

interface LegendChip {
  readonly series: Series;
  readonly label: string; // The counter's name, cropped if it didn't fit.
  readonly left: number;
  readonly right: number;
}

interface LegendLayout {
  readonly width: number; // Of the track.
  readonly chipWidths: ReadonlyArray<number>; // Of every chip, uncropped.
  readonly room: number; // For the chips of a page.
  readonly paged: boolean; // Whether the chips didn't all fit.
  readonly end: number; // The index after the last chip shown.
}

// Series colours, cycled through when there are more counters than colours.
// The theme's --pf-chart-color-N variables take precedence; these are used
// when the theme doesn't define them (e.g. in embedded builds).
const FALLBACK_PALETTE: ReadonlyArray<string> = [
  '#4285f4', // Blue
  '#ea4335', // Red
  '#fbbc04', // Yellow
  '#34a853', // Green
  '#ff6d01', // Orange
  '#46bdc6', // Cyan
  '#9334e6', // Purple
  '#185abc', // Dark blue
];

const BUCKETS_PER_PIXEL = 2;
// The height of the plot per unit of chart height.
const PLOT_HEIGHT_PX = 40;
// SQLite caps a compound SELECT at 500 terms, so queries with a UNION ALL term
// per counter are split into batches of this many counters.
const COUNTERS_PER_QUERY = 500;
const LEGEND_HEIGHT_PX = 14;
const LEGEND_PAD_PX = 4;
const LEGEND_SWATCH_PX = 8;
const LEGEND_GAP_PX = 10;
// The width of each of the arrows paging through a legend that doesn't fit.
const LEGEND_ARROW_PX = 14;
const PLOT_PAD_TOP = 9;
const PLOT_PAD_BOTTOM = 5;
const TICK_SPACING_PX = 34;
const AXIS_FONT = '10px Roboto Condensed';
const AREA_ALPHA_TOP = 0.32;
const AREA_ALPHA_BOTTOM = 0.02;
const BAND_ALPHA = 0.18;
const DIM_ALPHA = 0.2;
const HIDDEN_ALPHA = 0.35;
// How near the cursor must be to a line to pick it out.
const FOCUS_TOLERANCE_PX = 8;
// Counters have independent sample times, so the stacked layout resamples
// them onto columns this wide before summing.
const STACK_COLUMN_PX = 3;

// Makes the names of the mipmap tables unique across renderers.
let tableGeneration = 0;

// A setting picked from a list of options, rendered as radio items.
function radioDescriptor<T>(
  name: string,
  options: ReadonlyArray<readonly [string, T]>,
): TrackSettingDescriptor<T> {
  return {
    name,
    description: options.map(([label]) => label).join(', '),
    render(setter, values) {
      const value = valueIfAllEqual(values);
      const current = options.find(([, v]) => v === value)?.[0] ?? 'mixed';
      return m(
        MenuItem,
        {label: `${name} (currently: ${current})`},
        options.map(([label, v]) =>
          m(MenuItem, {
            label,
            onclick: () => setter(v),
            icon: value === v ? Icons.RadioChecked : Icons.RadioUnchecked,
          }),
        ),
      );
    },
  };
}

const LAYOUTS: ReadonlyArray<readonly [string, Layout]> = [
  ['Lines', 'lines'],
  ['Stacked area', 'stacked'],
];

const shareRowAxesDescriptor: TrackSettingDescriptor<boolean> = {
  name: 'Shared y-axis',
  description: 'Rows in the same unit share a y-axis',
  render(setter, values) {
    const value = valueIfAllEqual(values);
    return m(MenuItem, {
      label: 'Shared y-axis',
      icon:
        value === undefined
          ? Icons.IndeterminateCheckbox
          : value
            ? Icons.Checkbox
            : Icons.BlankCheckbox,
      onclick: () => setter(!value),
    });
  },
};
const yModeDescriptor = radioDescriptor<YMode>('Mode', [
  ['Value', 'value'],
  ['Delta', 'delta'],
  ['Rate', 'rate'],
]);
const yRangeDescriptor = radioDescriptor<YRange>('Y-axis range', [
  ['Whole trace', 'all'],
  ['Viewport', 'viewport'],
]);
const yDisplayDescriptor = radioDescriptor<YDisplay>('Y-axis display', [
  ['Zero-based', 'zero'],
  ['Min/Max', 'minmax'],
  ['Log', 'log'],
]);
const yRoundingDescriptor = radioDescriptor<YRounding>('Y-axis rounding', [
  ['Human readable', 'human_readable'],
  ['Strict', 'strict'],
]);
const chartHeightDescriptor = radioDescriptor<ChartHeight>('Chart height', [
  ['Small (1x)', 1],
  ['Medium (2x)', 2],
  ['Large (4x)', 4],
  ['XLarge (8x)', 8],
]);

class TimeseriesRenderer implements TrackRenderer {
  private readonly queue = new AtomicTaskQueue();
  private readonly tablesSlot = new AsyncMemo<MipmapTables>(this.queue);
  private readonly dataSlot = new AsyncMemo<ReadonlyArray<DataFrame>>(
    this.queue,
  );
  private readonly bufferedBounds = new BufferedBounds();
  private readonly series: ReadonlyArray<Series>;

  // Display settings, changed from the settings and track shell menus.
  private layout: Layout = 'lines';
  private expanded = false;
  private yMode: YMode;
  private yRange: YRange = 'all';
  private yDisplay: YDisplay = 'zero';
  private yRounding: YRounding = 'human_readable';
  // Whether expanded rows in the same unit share a y-axis, so their heights
  // compare.
  private shareRowAxes = true;
  private chartHeight: ChartHeight = 4;

  // The sample of each series under the cursor, from the last mouse move.
  private hovers: ReadonlyArray<HoverState | undefined> = [];

  // The series under the cursor. The others are dimmed.
  private focused?: Series;

  // Where the last render put the plot and its axis, for mapping the mouse
  // back onto a series.
  private plot = {top: 0, height: 0, yMin: 0, yMax: 0};

  // The legend chips drawn by the last render.
  private legendChips: ReadonlyArray<LegendChip> = [];

  // The first series with a chip on the legend's current page, and the
  // legend's layout in the last render.
  private legendStart = 0;
  private legendLayout?: LegendLayout;

  // The theme the series colours were read for; see updateColors().
  private colorsKey?: string;

  constructor(
    private readonly trace: Trace,
    counters: ReadonlyArray<TimeseriesCounter>,
    private readonly opts: TimeseriesRendererOpts,
  ) {
    this.yMode = opts.yMode ?? 'value';
    this.series = counters.map((counter, index) => ({
      counter,
      index,
      color: paletteColor(FALLBACK_PALETTE, index),
      hidden: false,
    }));
  }

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
        descriptor: yRangeDescriptor,
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
        descriptor: yRoundingDescriptor,
        value: this.yRounding,
        update: (value) => {
          this.yRounding = value;
        },
      }),
      setting({
        descriptor: shareRowAxesDescriptor,
        value: this.shareRowAxes,
        update: (value) => {
          this.shareRowAxes = value;
        },
      }),
      setting({
        descriptor: chartHeightDescriptor,
        value: this.chartHeight,
        update: (value) => {
          this.chartHeight = value;
        },
      }),
    ];
  }

  getHeight(): number {
    if (this.expanded) {
      const rows = Math.max(1, this.visibleSeries().length);
      return LEGEND_HEIGHT_PX + rows * this.rowHeightPx;
    }
    return LEGEND_HEIGHT_PX + PLOT_HEIGHT_PX * this.chartHeight;
  }

  getTrackShellButtons(): m.Children {
    return [
      m(Button, {
        className: 'pf-visible-on-hover',
        onclick: () => this.setExpanded(!this.expanded),
        icon: this.expanded ? Icons.UnfoldLess : Icons.UnfoldMore,
        tooltip: this.expanded
          ? 'Show as one plot'
          : 'Show each counter in its own row',
        compact: true,
      }),
      m(
        PopupMenu,
        {
          trigger: m(Button, {
            className: 'pf-visible-on-hover',
            icon: 'legend_toggle',
            tooltip: 'Layout and counters',
            compact: true,
          }),
        },
        this.renderMenuItems(),
      ),
    ];
  }

  render(trackCtx: TrackRenderContext): void {
    this.updateColors(trackCtx);
    const loaded = this.useData(trackCtx);
    this.drawLegend(trackCtx);

    const visible = this.visibleSeries();
    if (loaded && visible.length > 0) {
      const top = LEGEND_HEIGHT_PX;
      const height = trackCtx.size.height - top;
      switch (this.view) {
        case 'lines':
          this.drawLines(trackCtx, visible, top, height);
          break;
        case 'stacked':
          this.drawStacked(trackCtx, visible, top, height);
          break;
        case 'rows': {
          this.plot = {top, height, yMin: 0, yMax: 0};
          const rowHeight = height / visible.length;
          const shared = this.shareRowAxes ? limitsByUnit(visible) : undefined;
          visible.forEach((series, i) => {
            this.drawRow(
              trackCtx,
              series,
              top + i * rowHeight,
              rowHeight,
              shared?.get(series.counter.unit ?? ''),
            );
          });
          break;
        }
        default:
          assertUnreachable(this.view);
      }
    }

    this.drawLoading(trackCtx);
  }

  onMouseMove({x, y, timescale}: TrackMouseEvent) {
    const hovers = this.series.map((s) =>
      s.hidden ? undefined : findSampleAtX(s.frame, x, timescale),
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

  // Clicking a legend arrow pages the legend, and clicking a chip shows or
  // hides its counter.
  onMouseClick({x, y}: TrackMouseEvent): boolean {
    const arrow = this.legendArrowAt(x, y);
    if (arrow !== undefined) {
      this.pageLegend(arrow);
      return true;
    }
    const chip = this.legendChipAt(x, y);
    if (chip === undefined) return false;
    this.setHidden(chip.series, !chip.series.hidden);
    m.redraw(); // Hiding a row changes the height of an expanded track.
    return true;
  }

  // The counter under the cursor, and the total of all shown counters.
  renderTooltip(): m.Children {
    const visible = this.visibleSeries();
    const hovered = visible.filter((s) => this.hovers[s.index] !== undefined);
    if (hovered.length === 0) return undefined;

    const rows: m.Children[] = [];
    const focused = this.focused;
    const focusedHover = focused && this.hovers[focused.index];
    if (focused && focusedHover) {
      rows.push(
        m(ChartTooltip.Row, {
          name: focused.counter.name,
          value: this.formatValue(focusedHover.value, focused.counter.unit),
          swatch: focused.color.cssString,
        }),
      );
    }
    // Summing is only meaningful for values in one unit on a linear scale.
    const unit = sharedUnit(visible);
    if (!this.isLog && unit !== undefined) {
      let total = 0;
      for (const s of hovered) {
        total += this.hovers[s.index]?.value ?? 0;
      }
      rows.push(
        m(ChartTooltip.Row, {
          name: 'Total',
          value: this.formatValue(total, unit),
          tweak: focused && 'muted',
        }),
      );
    }
    return rows.length > 0 ? rows : undefined;
  }

  // -- Private methods --

  // What is drawn: the layout's single plot, or a row per counter.
  private get view(): Layout | 'rows' {
    return this.expanded ? 'rows' : this.layout;
  }

  private get rowHeightPx(): number {
    return PLOT_HEIGHT_PX * Math.max(1, this.chartHeight / 2);
  }

  // Stacking sums the values, which is meaningless for their logs.
  private get isLog(): boolean {
    return this.yDisplay === 'log' && this.view !== 'stacked';
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

  private setExpanded(expanded: boolean) {
    this.expanded = expanded;
    this.hovers = [];
    this.focused = undefined;
  }

  // Picks the series colours from the theme's chart palette. It's re-read only
  // when the theme's canvas colours change, as getComputedStyle isn't cheap.
  private updateColors({ctx, colors}: TrackRenderContext) {
    const key = colors.COLOR_BACKGROUND;
    if (key === this.colorsKey) return;
    this.colorsKey = key;
    const themed =
      ctx.canvas instanceof Element
        ? getChartThemeColors(ctx.canvas).chartColors
        : [];
    const palette = FALLBACK_PALETTE.map((fallback, i) =>
      /^#[0-9a-f]{6}$/i.test(themed[i] ?? '') ? themed[i] : fallback,
    );
    for (const s of this.series) {
      s.color = paletteColor(palette, s.index);
    }
  }

  private renderMenuItems(): m.Children {
    return [
      LAYOUTS.map(([label, layout]) =>
        m(MenuItem, {
          label,
          icon:
            this.layout === layout ? Icons.RadioChecked : Icons.RadioUnchecked,
          onclick: () => {
            this.layout = layout;
            this.setExpanded(false);
          },
        }),
      ),
      m(MenuDivider),
      m(MenuItem, {
        label: 'Show all counters',
        icon: 'visibility',
        disabled: this.series.every((s) => !s.hidden),
        closePopupOnClick: false,
        onclick: () => this.series.forEach((s) => this.setHidden(s, false)),
      }),
      m(MenuItem, {
        label: 'Hide all counters',
        icon: Icons.Hide,
        disabled: this.series.every((s) => s.hidden),
        closePopupOnClick: false,
        onclick: () => this.series.forEach((s) => this.setHidden(s, true)),
      }),
      m(
        MenuItem,
        {label: 'Counters', icon: 'legend_toggle'},
        m(
          '.pf-timeseries-track__counters',
          this.series.map((s) =>
            m(MenuItem, {
              label: s.counter.name,
              icon: s.hidden ? Icons.BlankCheckbox : Icons.Checkbox,
              closePopupOnClick: false,
              onclick: () => this.setHidden(s, !s.hidden),
            }),
          ),
        ),
      ),
      this.opts.extraMenuItems && [m(MenuDivider), this.opts.extraMenuItems()],
    ];
  }

  private alphaOf(series: Series): number {
    return this.focused === undefined || this.focused === series
      ? 1
      : DIM_ALPHA;
  }

  // The series the cursor picks out: the chip it is over in the legend, and
  // otherwise the line it is on, the band it is in, or the row it is in.
  private seriesAt(
    x: number,
    y: number,
    hovers: ReadonlyArray<HoverState | undefined>,
  ): Series | undefined {
    if (y < LEGEND_HEIGHT_PX) {
      const series = this.legendChipAt(x, y)?.series;
      return series?.hidden ? undefined : series;
    }

    const visible = this.visibleSeries().filter(
      (s) => hovers[s.index] !== undefined,
    );
    const {top, height, yMin, yMax} = this.plot;
    const valueOf = (s: Series) => hovers[s.index]?.value ?? 0;
    switch (this.view) {
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
      case 'rows': {
        const shown = this.visibleSeries();
        const row = Math.floor(((y - top) / height) * shown.length);
        return shown[row];
      }
      default:
        assertUnreachable(this.view);
    }
  }

  private legendChipAt(x: number, y: number): LegendChip | undefined {
    if (y >= LEGEND_HEIGHT_PX) return undefined;
    return this.legendChips.find((c) => x >= c.left && x <= c.right);
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
    const {ctx, size, timescale} = trackCtx;

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
    const yMax = this.yRounding === 'human_readable' ? roundUp(total) : total;
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
      const color = series.color;
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
  private drawRow(
    trackCtx: TrackRenderContext,
    series: Series,
    top: number,
    height: number,
    sharedLimits?: {global: Limits; viewport: Limits},
  ) {
    const {ctx, size, colors} = trackCtx;
    const limits = series.frame?.limits ?? {min: 0, max: 0};
    const {yMin, yMax} = this.computeYRange(
      sharedLimits?.global ?? series.globalLimits ?? limits,
      sharedLimits?.viewport ?? limits,
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

    const name = series.counter.name;
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
    const {ctx, size, timescale} = trackCtx;
    const data = series.frame;
    if (data === undefined || data.timestampsRel.length === 0) return;

    const color = series.color;
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
    const unit = sharedUnit(series);
    return values.map((value) => ({
      value,
      label: this.formatValue(value, unit),
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

  // A chip per series, struck through when hidden. When they don't all fit,
  // arrows at either end page through them. (Tracks can't handle the wheel,
  // so the legend can't scroll.)
  private drawLegend({ctx, size, colors}: TrackRenderContext) {
    ctx.save();
    ctx.font = AXIS_FONT;
    ctx.textAlign = 'left';
    ctx.textBaseline = 'middle';
    const midY = LEGEND_HEIGHT_PX / 2;

    const labelX = LEGEND_SWATCH_PX + 4; // From a chip's left edge.
    const chipWidths = this.series.map(
      (s) => labelX + ctx.measureText(s.counter.name).width,
    );
    const n = chipWidths.length;
    const paged = fitChips(chipWidths, 0, size.width - 2 * LEGEND_PAD_PX) < n;
    const room = paged
      ? size.width - 2 * LEGEND_ARROW_PX
      : size.width - 2 * LEGEND_PAD_PX;
    const start = paged ? Math.min(this.legendStart, n - 1) : 0;
    const end = fitChips(chipWidths, start, room);
    this.legendStart = start;
    this.legendLayout = {width: size.width, chipWidths, room, paged, end};

    const chips: LegendChip[] = [];
    let left = paged ? LEGEND_ARROW_PX : LEGEND_PAD_PX;
    for (let i = start; i < end; i++) {
      const series = this.series[i];
      const name = series.counter.name;
      // Only a chip alone on its page can be too wide: crop its label.
      const textWidth = chipWidths[i] - labelX;
      const maxWidth = room - labelX;
      const label =
        textWidth > maxWidth
          ? cropText(name, textWidth / name.length, maxWidth)
          : name;
      const right = left + labelX + ctx.measureText(label).width;
      chips.push({series, label, left, right});
      left = right + LEGEND_GAP_PX;
    }
    this.legendChips = chips;

    for (const {series, label, left, right} of chips) {
      const textX = left + labelX;
      ctx.globalAlpha = series.hidden ? HIDDEN_ALPHA : 1;
      ctx.fillStyle = series.color.cssString;
      ctx.fillRect(
        left,
        midY - LEGEND_SWATCH_PX / 2,
        LEGEND_SWATCH_PX,
        LEGEND_SWATCH_PX,
      );
      ctx.fillStyle = colors.COLOR_TEXT;
      ctx.fillText(label, textX, midY);
      if (series.hidden) {
        ctx.strokeStyle = colors.COLOR_TEXT;
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(textX, midY);
        ctx.lineTo(right, midY);
        ctx.stroke();
      }
    }

    if (paged) {
      const color = colors.COLOR_TEXT;
      const arrowMid = LEGEND_ARROW_PX / 2;
      drawChevron(ctx, arrowMid, midY, -1, color, start > 0);
      drawChevron(ctx, size.width - arrowMid, midY, 1, color, end < n);
    }
    ctx.restore();
  }

  // The legend arrow at x, if the legend is paged: -1 back, 1 forward.
  private legendArrowAt(x: number, y: number): -1 | 1 | undefined {
    const layout = this.legendLayout;
    if (y >= LEGEND_HEIGHT_PX || layout === undefined || !layout.paged) {
      return undefined;
    }
    if (x < LEGEND_ARROW_PX) return -1;
    if (x > layout.width - LEGEND_ARROW_PX) return 1;
    return undefined;
  }

  // Shows the previous (-1) or next (1) page of legend chips.
  private pageLegend(dir: -1 | 1) {
    const layout = this.legendLayout;
    if (layout === undefined) return;
    const {chipWidths, room, end} = layout;
    if (dir > 0) {
      if (end < chipWidths.length) this.legendStart = end;
    } else {
      this.legendStart = previousPageStart(chipWidths, this.legendStart, room);
    }
  }

  // If the cached data doesn't fully cover the visible time range, show a
  // checkerboard under the legend.
  private drawLoading({ctx, size, timescale}: TrackRenderContext) {
    const {start, end} = this.bufferedBounds.bounds;
    ctx.save();
    ctx.translate(0, LEGEND_HEIGHT_PX);
    checkerboardExcept(
      ctx,
      size.height - LEGEND_HEIGHT_PX,
      0,
      size.width,
      timescale.timeToPx(start),
      timescale.timeToPx(end),
    );
    ctx.restore();
  }

  // Whether the samples for the current window are loaded.
  private useData(trackCtx: TrackRenderContext): boolean {
    const {size, visibleWindow} = trackCtx;

    // Step 1: The mipmap tables, rebuilt when the values they hold change.
    const {yMode, isLog} = this;
    const tables = this.tablesSlot.use({
      key: {yMode, isLog},
      compute: () => this.createMipmapTables(yMode, isLog),
    }).data;
    if (tables === undefined) return false;
    this.series.forEach((s, i) => (s.globalLimits = tables.globalLimits[i]));

    // Step 2: The buffered bounds to fetch for the visible window.
    const visibleSpan = visibleWindow.toTimeSpan();
    const bucketSize = computeBucketSize(
      visibleSpan.duration,
      Math.max(1, size.width),
    );
    const bounds = this.bufferedBounds.update(visibleSpan, bucketSize);

    // Step 3: Every counter's samples, in one query. Hidden counters are
    // fetched too, so showing one again is instant.
    const {tableNames} = tables;
    const frames = this.dataSlot.use({
      key: {
        start: bounds.start,
        end: bounds.end,
        resolution: bounds.resolution,
        tableNames,
      },
      compute: (signal) =>
        this.trace.taskTracker.track(
          this.fetchData(
            tableNames,
            bounds.start,
            bounds.end,
            bounds.resolution,
            signal,
          ),
          'Loading timeseries',
        ),
      retainOn: ['start', 'end', 'resolution'],
    }).data;
    if (frames === undefined) return false;
    this.series.forEach((s, i) => (s.frame = frames[i]));
    return true;
  }

  // Compute the range of values to display.
  private computeYRange(
    globalLimits: Limits, // Over the whole trace.
    viewportLimits: Limits, // Over the loaded window.
  ): {yMin: number; yMax: number} {
    const limits = this.yRange === 'viewport' ? viewportLimits : globalLimits;
    let yMin = limits.min;
    let yMax = limits.max;
    if (this.yDisplay === 'zero') {
      yMin = Math.min(0, yMin);
      yMax = Math.max(0, yMax);
    }
    if (this.yRounding === 'human_readable') {
      if (this.isLog) {
        yMax = Math.log(roundUp(Math.exp(yMax)));
        yMin = Math.log(roundDown(Math.exp(yMin)));
      } else {
        yMax = roundUp(yMax);
        yMin = roundDown(yMin);
      }
    }
    return {yMin, yMax};
  }

  // Formats a value (or its log) in the current mode, in the given unit.
  private formatValue(value: number, unit = ''): string {
    const v = this.isLog ? Math.exp(value) : value;
    switch (this.yMode) {
      case 'value':
        return formatNumber(v, unit);
      case 'delta':
        return `\u0394${formatNumber(v, unit)}`;
      case 'rate':
        return formatNumber(v, `\u0394${unit}/s`);
      default:
        assertUnreachable(this.yMode);
    }
  }

  // Creates a mipmap table per counter in one query, then reads their global
  // limits in one query per batch of counters.
  private async createMipmapTables(
    yMode: YMode,
    log: boolean,
  ): Promise<MipmapTables> {
    const engine = this.trace.engine;
    const prefix = `__timeseries_mipmap_${tableGeneration++}`;
    const tableNames = this.series.map((_, i) => `${prefix}_${i}`);
    const valueExpr = log
      ? `ifnull(ln(${counterValueExpression(yMode)}), 0)`
      : counterValueExpression(yMode);
    await engine.query(
      this.series
        .map(
          ({counter}, i) => `
            CREATE VIRTUAL TABLE ${tableNames[i]}
            USING __intrinsic_counter_mipmap((
              SELECT ts, ${valueExpr} AS value
              FROM counter
              WHERE track_id = ${counter.id}
            ));`,
        )
        .join('\n'),
    );

    // A counter with no samples has no row, and keeps empty limits.
    const globalLimits: Limits[] = tableNames.map(() => ({min: 0, max: 0}));
    for (const [offset, batch] of batches(tableNames)) {
      const result = await engine.query(
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
      const it = result.iter({seriesIndex: NUM, min: NUM, max: NUM});
      for (; it.valid(); it.next()) {
        globalLimits[it.seriesIndex] = {min: it.min, max: it.max};
      }
    }

    return {
      tableNames,
      globalLimits,
      [Symbol.asyncDispose]: async () => {
        await engine.query(
          tableNames.map((name) => `DROP TABLE IF EXISTS ${name};`).join('\n'),
        );
      },
    };
  }

  // Fetches every table's samples for the given bounds, in one query per
  // batch of tables.
  private async fetchData(
    tableNames: ReadonlyArray<string>,
    start: time,
    end: time,
    resolution: duration,
    signal: CancellationSignal,
  ): Promise<ReadonlyArray<DataFrame>> {
    const frames: DataFrame[] = [];
    for (const [, batch] of batches(tableNames)) {
      const result = await this.trace.engine.query(`
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

      const cols = result.decodeColumns({
        tableIndex: NUM,
        tsRel: NUM,
        minDisplayValue: NUM,
        maxDisplayValue: NUM,
        lastDisplayValue: NUM,
      });

      // Rows are grouped by table and in time order within each.
      const numRows = result.numRows();
      let row = 0;
      for (let i = 0; i < batch.length; i++) {
        const from = row;
        while (row < numRows && cols.tableIndex[row] === i) row++;
        frames.push(buildDataFrame(cols, from, row, start, end));
      }
    }
    return frames;
  }
}

// Fits legend chips of the given widths into `room` px, starting from chip
// `start`. Returns the index after the last chip that fits. The first always
// does: its label gets cropped.
function fitChips(
  widths: ReadonlyArray<number>,
  start: number,
  room: number,
): number {
  let used = 0;
  let end = start;
  for (; end < widths.length; end++) {
    const width = widths[end] + (end > start ? LEGEND_GAP_PX : 0);
    if (end > start && used + width > room) break;
    used += width;
  }
  return end;
}

// The first chip of the page before the one starting at chip `start`: as many
// chips before it as fit into `room` px, and at least one.
function previousPageStart(
  widths: ReadonlyArray<number>,
  start: number,
  room: number,
): number {
  let used = 0;
  let first = start;
  for (; first > 0; first--) {
    const width = widths[first - 1] + (first < start ? LEGEND_GAP_PX : 0);
    if (first < start && used + width > room) break;
    used += width;
  }
  return first;
}

// A chevron centred on (x, y) pointing left (-1) or right (1), faded when
// disabled.
function drawChevron(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  dir: -1 | 1,
  color: string,
  enabled: boolean,
) {
  ctx.globalAlpha = enabled ? 1 : HIDDEN_ALPHA;
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  ctx.moveTo(x - 2 * dir, y - 4);
  ctx.lineTo(x + 2 * dir, y);
  ctx.lineTo(x - 2 * dir, y + 4);
  ctx.stroke();
}

// The colour of the i-th series, cycling through the palette.
function paletteColor(palette: ReadonlyArray<string>, i: number): HSLColor {
  return new HSLColor(palette[i % palette.length]);
}

// The unit all of the series share ('' if none has one), or undefined if
// they differ.
function sharedUnit(series: ReadonlyArray<Series>): string | undefined {
  return valueIfAllEqual(series.map((s) => s.counter.unit ?? ''));
}

// Splits items into batches of COUNTERS_PER_QUERY, each with its offset.
function batches<T>(items: ReadonlyArray<T>): Array<[number, T[]]> {
  const result: Array<[number, T[]]> = [];
  for (let i = 0; i < items.length; i += COUNTERS_PER_QUERY) {
    result.push([i, items.slice(i, i + COUNTERS_PER_QUERY)]);
  }
  return result;
}

// Buckets about BUCKETS_PER_PIXEL to a pixel, rounded to a power of 2.
function computeBucketSize(
  spanDuration: duration,
  windowSizePx: number,
): duration {
  const nsPerPx = Math.max(1, Number(spanDuration) / windowSizePx);
  const bucketNs = nsPerPx / BUCKETS_PER_PIXEL;
  const exp = Math.ceil(Math.log2(Math.max(1, bucketNs)));
  return BigInt(Math.pow(2, exp)) as duration;
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
    limits: {min, max},
  };
}

// The sample a series holds at pixel x, if any.
function findSampleAtX(
  data: DataFrame | undefined,
  x: number,
  timescale: TimeScale,
): HoverState | undefined {
  if (data === undefined) return undefined;
  const relNs = Number(timescale.pxToHpTime(x).toTime() - data.start);
  const [left] = searchSegment(data.timestampsRel, relNs);
  if (left === -1) return undefined;
  return {
    tsRel: data.timestampsRel[left],
    value: data.lastDisplayValues[left],
  };
}

// The value a series holds at pixel x, or 0 before its first sample.
function valueAtX(
  data: DataFrame | undefined,
  x: number,
  timescale: TimeScale,
): number {
  return findSampleAtX(data, x, timescale)?.value ?? 0;
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

// The combined whole-trace and on-screen range of the rows in each unit.
function limitsByUnit(
  series: ReadonlyArray<Series>,
): Map<string, {global: Limits; viewport: Limits}> {
  const groups = new Map<string, Series[]>();
  for (const s of series) {
    const unit = s.counter.unit ?? '';
    const group = groups.get(unit) ?? [];
    group.push(s);
    groups.set(unit, group);
  }
  const result = new Map<string, {global: Limits; viewport: Limits}>();
  for (const [name, group] of groups) {
    result.set(name, {
      global: unionLimits(group.map((s) => s.globalLimits)),
      viewport: unionLimits(group.map((s) => s.frame?.limits)),
    });
  }
  return result;
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
