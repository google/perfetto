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
import {SimpleResizeObserver} from '../../../base/resize_observer';
import {classNames} from '../../../base/classnames';
import {Spinner} from '../../../widgets/spinner';
import type {
  LineChartAttrs,
  LineChartData,
  LineChartSeries,
} from '../charts/line_chart';
import {clamp} from '../../../base/math_utils';
import {max, min} from '../../../base/array_utils';
import {renderLegend} from './legend';
import {renderTooltip} from './tooltip';
import {shortUuid} from '../../../base/uuid';
import {
  AxisRange,
  BORDER_COLOR,
  TEXT_COLOR,
  chartColorVar,
  defaultFmt,
  estimateLabelWidth,
  logRange,
  niceRange,
  pointMarker,
  rangeWithFixedBounds,
} from './common';
import {assertIsInstance} from '../../../base/assert';

export type {
  LineChartAttrs,
  LineChartData,
  LineChartSeries,
} from '../charts/line_chart';

const DEFAULT_HEIGHT = 200;
const AXIS_LABEL_FONT_SIZE = 10;
const AXIS_NAME_FONT_SIZE = 11;
const TICK_LENGTH = 4;
const TICK_LABEL_GAP = 4;

interface SeriesPlot {
  readonly name: string;
  readonly color: string;
  readonly points: ReadonlyArray<{x: number; y: number; raw: number}>;
}

interface HoverState {
  readonly clientX: number;
  readonly clientY: number;
  readonly index: number;
  readonly xValue: number;
}

export class LineChartSvg implements m.ClassComponent<LineChartAttrs> {
  private resizeObs?: Disposable;
  private hover?: HoverState;
  // Index of the currently-hovered series, if any. Used to dim other series.
  private hoveredSeries?: number;
  private container?: HTMLElement;
  private currentAttrs?: LineChartAttrs;
  private prevW = 0;
  private prevH = 0;
  // Active brush drag state, in *data* x-coordinates so a resize during the
  // drag doesn't mangle the selection. Pointer capture keeps the drag bound
  // to the SVG even when the cursor leaves it, so we don't need any
  // window-level listeners.
  private brushing?: {start: number; current: number; pointerId: number};
  // Unique id for the plot-area clipPath. Multiple charts on the same page
  // must not share an id.
  private readonly clipId = `pf-chart-clip-${shortUuid()}`;
  // Series the user has toggled off via legend click, keyed by series name.
  private hiddenSeries = new Set<string>();

  private toggleSeries(name: string) {
    if (this.hiddenSeries.has(name)) {
      this.hiddenSeries.delete(name);
    } else {
      this.hiddenSeries.add(name);
    }
  }

  oncreate({dom, attrs}: m.CVnodeDOM<LineChartAttrs>) {
    const container = dom.querySelector(
      '.pf-chart-svg__container',
    ) as HTMLElement;
    this.container = container;
    this.currentAttrs = attrs;
    const {width, height} = container.getBoundingClientRect();
    this.prevW = width;
    this.prevH = height;
    this.renderInner(container, attrs, width, height);
    this.resizeObs = new SimpleResizeObserver(dom, () => {
      if (!this.container || !this.currentAttrs) return;
      const r = this.container.getBoundingClientRect();
      if (r.width === this.prevW && r.height === this.prevH) return;
      this.prevW = r.width;
      this.prevH = r.height;
      this.renderInner(this.container, this.currentAttrs, r.width, r.height);
    });
  }

  onupdate({dom, attrs}: m.CVnodeDOM<LineChartAttrs>) {
    // Re-query: Mithril may have replaced the container node if siblings
    // changed (e.g. legend appeared/disappeared).
    const container = assertIsInstance(
      dom.querySelector('.pf-chart-svg__container'),
      HTMLElement,
    );
    this.container = container;
    this.currentAttrs = attrs;
    const {width, height} = container.getBoundingClientRect();
    this.prevW = width;
    this.prevH = height;
    this.renderInner(container, attrs, width, height);
  }

  private setHoveredSeries(i: number) {
    if (this.hoveredSeries !== i) {
      this.hoveredSeries = i;
    }
  }

  private clearHoveredSeries(i: number) {
    if (this.hoveredSeries === i) {
      this.hoveredSeries = undefined;
    }
  }

  onremove() {
    if (this.resizeObs) {
      this.resizeObs[Symbol.dispose]();
      this.resizeObs = undefined;
    }
    // Tear down the imperative inner tree so any descendant onremove hooks
    // run.
    if (this.container) {
      m.render(this.container, []);
    }
  }

  // Imperatively render the inner chart tree using measured dimensions.
  private renderInner(
    container: HTMLElement,
    attrs: LineChartAttrs,
    width: number,
    height: number,
  ) {
    const {data} = attrs;
    const isEmpty =
      data !== undefined &&
      (data.series.length === 0 ||
        data.series.every((s) => s.points.length === 0));
    const isLoading = data === undefined;

    let inner: m.Children;
    if (isLoading) {
      inner = m('.pf-chart-svg__loading', m(Spinner));
    } else if (isEmpty) {
      inner = m('.pf-chart-svg__empty', 'No data to display');
    } else {
      inner = this.renderChart(attrs, data, width, height);
    }

    // Mithril's render() function actually takes a hidden third parameter which
    // is a function called whenever an event triggers a redraw from within the
    // rendered vnodes. Here we can use this to hook up the events triggered
    // from within the rendered SVG code to trigger a global redraw. This is the
    // same technique used in raf.ts.
    const renderWithRedraw = m.render as (
      el: Element,
      vnodes: m.Children,
      redraw?: () => void,
    ) => void;

    renderWithRedraw(container, inner, m.redraw);
  }

  view({attrs}: m.Vnode<LineChartAttrs>) {
    const {height, fillParent, className, data} = attrs;
    const legendPosition = attrs.legendPosition ?? 'top';
    const showLegend =
      attrs.showLegend ?? (data !== undefined && data.series.length > 1);

    // The contents of .pf-chart-svg__container are managed imperatively
    // via m.render() in onupdate/oncreate so that the chart is drawn using
    // measured dimensions on the very first paint and on every attrs change.
    // The legend is plain Mithril and lives outside the imperative subtree.
    const legend =
      showLegend &&
      data !== undefined &&
      renderLegend(
        data,
        attrs.formatYValue ?? defaultFmt,
        this.hiddenSeries,
        (name: string) => this.toggleSeries(name),
      );
    const chart = m('.pf-chart-svg__container', {key: 'chart'});

    const seriesHover = data?.series
      .filter((s) => !this.hiddenSeries.has(s.name))
      .map((s, i) => {
        const isHovered = this.hoveredSeries === i;
        const isMuted = this.hoveredSeries !== undefined && !isHovered;
        return {
          series: s,
          style: isHovered ? 'emphasis' : isMuted ? 'muted' : 'normal',
        };
      });

    const tooltip =
      this.hover !== undefined &&
      seriesHover !== undefined &&
      renderTooltip(
        attrs.stacked ? [...seriesHover].reverse() : seriesHover,
        this.hover.index,
        attrs.formatXValue ?? defaultFmt,
        attrs.formatYValue ?? defaultFmt,
      );

    return m(
      '.pf-line-chart-svg',
      {
        className: classNames(
          fillParent && 'pf-chart-svg--fill-parent',
          `pf-chart-svg--legend-${legendPosition}`,
          className,
        ),
        style: fillParent
          ? undefined
          : {height: `${height ?? DEFAULT_HEIGHT}px`},
      },
      // Removes falsy children - mithril doesn't like it when some children
      // have keys while others are null.
      [chart, legend, tooltip].filter(Boolean),
    );
  }

  private renderChart(
    attrs: LineChartAttrs,
    data: LineChartData,
    width: number,
    height: number,
  ) {
    if (width === 0 || height === 0) {
      return m('svg', {width: '100%', height: '100%'});
    }

    const stacked = attrs.stacked ?? false;
    const showPoints = attrs.showPoints ?? true;
    const lineWidth = attrs.lineWidth ?? 2;
    const fmtX = attrs.formatXValue ?? defaultFmt;
    const fmtY = attrs.formatYValue ?? defaultFmt;
    const gridLines = attrs.gridLines;
    const showHGrid = gridLines === 'horizontal' || gridLines === 'both';
    const showVGrid = gridLines === 'vertical' || gridLines === 'both';
    const logScale = attrs.logScale ?? false;
    const scaleAxes = attrs.scaleAxes ?? false;
    const integerX = attrs.integerX ?? false;
    const integerY = attrs.integerY ?? false;
    const yMinInterval = attrs.yAxisMinInterval;

    // Compute series in chart-space (post-stacking if stacked). Series
    // toggled off via the legend are excluded entirely.
    const visibleSeries = data.series.filter(
      (s) => !this.hiddenSeries.has(s.name),
    );
    const seriesPlots = buildSeriesPlots(visibleSeries, stacked);

    // Compute axis ranges from the (post-stacking) values.
    const allX: number[] = [];
    const allY: number[] = [];
    for (const s of seriesPlots) {
      for (const p of s.points) {
        allX.push(p.x);
        allY.push(p.y);
      }
    }
    // By default, force-include zero on the y-axis. `scaleAxes: true` opts
    // out and computes the range from data min/max only.
    if (!scaleAxes) {
      allY.push(0);
    }

    const xMin = min(allX) ?? NaN;
    const xMax = max(allX) ?? NaN;
    const yMin = min(allY) ?? NaN;
    const yMax = max(allY) ?? NaN;
    const xRange =
      attrs.xAxisMin !== undefined || attrs.xAxisMax !== undefined
        ? rangeWithFixedBounds(attrs.xAxisMin ?? xMin, attrs.xAxisMax ?? xMax)
        : niceRange(xMin, xMax, {integer: integerX});
    const yRange = logScale
      ? logRange(
          // Log scale needs strictly-positive bounds; clamp data to a small
          // floor.
          Math.max(min(allY.filter((v) => v > 0)) ?? NaN, 1e-9),
          Math.max(yMax, 1e-9),
        )
      : niceRange(yMin, yMax, {
          integer: integerY,
          minInterval: yMinInterval,
        });

    // Layout: axis names + tick labels eat into the canvas.
    const xName = attrs.xAxisLabel;
    const yName = attrs.yAxisLabel;
    const yLabelWidth = estimateLabelWidth(yRange.ticks.map(fmtY));
    const padLeft = (yName ? AXIS_NAME_FONT_SIZE + 6 : 0) + yLabelWidth + 8;
    const padRight = 8;
    const padTop = 8;
    const padBottom =
      (xName ? AXIS_NAME_FONT_SIZE + 6 : 0) + AXIS_LABEL_FONT_SIZE + 8;

    const plotW = Math.max(0, width - padLeft - padRight);
    const plotH = Math.max(0, height - padTop - padBottom);

    const xToPx = (x: number) =>
      padLeft + ((x - xRange.min) / (xRange.max - xRange.min || 1)) * plotW;
    const yToPx = logScale
      ? (y: number) => {
          // Clamp to a tiny positive value so log() stays finite for zeros.
          const v = y > 0 ? y : yRange.min;
          const num = Math.log10(v) - Math.log10(yRange.min);
          const den = Math.log10(yRange.max) - Math.log10(yRange.min) || 1;
          return padTop + plotH - (num / den) * plotH;
        }
      : (y: number) =>
          padTop +
          plotH -
          ((y - yRange.min) / (yRange.max - yRange.min || 1)) * plotH;

    // Tooltip: snap to nearest x in the first series.
    const hoverIdx = this.hover?.index;

    return m(
      'svg.pf-chart-svg__svg',
      {
        width: width,
        height: height,
        viewBox: `0 0 ${width} ${height}`,
        style: attrs.onBrush && {cursor: 'crosshair'},
        oncontextmenu: (e: Event) => {
          // Chrome has a bug where right click to bring up the context menu
          // breaks pointer capture - simply disable context menus for the
          // chart.
          e.preventDefault();
        },
        onpointerdown:
          attrs.onBrush &&
          ((e: PointerEvent) =>
            this.handleBrushDown(e, padLeft, plotW, xRange)),
        onpointermove: (e: PointerEvent) =>
          this.handlePointerMove(e, seriesPlots, padLeft, plotW, xRange),
        onpointerup:
          attrs.onBrush &&
          ((e: PointerEvent) => this.handleBrushUp(e, attrs.onBrush!)),
        onpointerleave: () => {
          if (this.brushing) return; // capture keeps the drag alive.
          if (this.hover !== undefined) {
            this.hover = undefined;
          }
        },
        onlostpointercapture: () => {
          // Pointer capture yanked (e.g. system gesture) — abandon the drag.
          this.brushing = undefined;
        },
      },
      // Clip data drawings to the plot area so lines/points can't extend
      // past the axes when values fall outside the visible range.
      m(
        'defs',
        m(
          'clipPath',
          {id: this.clipId},
          m('rect', {x: padLeft, y: padTop, width: plotW, height: plotH}),
        ),
      ),
      // Y axis name (rotated)
      yName &&
        m(
          'text',
          {
            'x': AXIS_NAME_FONT_SIZE,
            'y': padTop + plotH / 2,
            'fill': TEXT_COLOR,
            'font-size': AXIS_NAME_FONT_SIZE,
            'text-anchor': 'middle',
            'transform': `rotate(-90 ${AXIS_NAME_FONT_SIZE} ${padTop + plotH / 2})`,
          },
          yName,
        ),
      // X axis name
      xName &&
        m(
          'text',
          {
            'x': padLeft + plotW / 2,
            'y': height - 4,
            'fill': TEXT_COLOR,
            'font-size': AXIS_NAME_FONT_SIZE,
            'text-anchor': 'middle',
          },
          xName,
        ),
      // Plot border + axes
      m('line', {
        x1: padLeft,
        y1: padTop + plotH,
        x2: padLeft + plotW,
        y2: padTop + plotH,
        stroke: BORDER_COLOR,
      }),
      m('line', {
        x1: padLeft,
        y1: padTop,
        x2: padLeft,
        y2: padTop + plotH,
        stroke: BORDER_COLOR,
      }),
      // Gridlines (drawn before series so they sit underneath).
      showHGrid &&
        yRange.ticks.map((t) =>
          m('line', {
            'x1': padLeft,
            'y1': yToPx(t),
            'x2': padLeft + plotW,
            'y2': yToPx(t),
            'stroke': BORDER_COLOR,
            'stroke-opacity': 0.3,
          }),
        ),
      showVGrid &&
        xRange.ticks.map((t) =>
          m('line', {
            'x1': xToPx(t),
            'y1': padTop,
            'x2': xToPx(t),
            'y2': padTop + plotH,
            'stroke': BORDER_COLOR,
            'stroke-opacity': 0.3,
          }),
        ),
      // Static selection overlay (driven by attrs.selection).
      attrs.selection !== undefined &&
        m('rect', {
          'x': xToPx(clamp(attrs.selection.start, xRange.min, xRange.max)),
          'y': padTop,
          'width': Math.max(
            0,
            xToPx(clamp(attrs.selection.end, xRange.min, xRange.max)) -
              xToPx(clamp(attrs.selection.start, xRange.min, xRange.max)),
          ),
          'height': plotH,
          'fill': 'rgba(0, 120, 212, 0.08)',
          'stroke': 'rgba(0, 120, 212, 0.3)',
          'stroke-width': 1,
          'pointer-events': 'none',
        }),
      // Y ticks + labels
      yRange.ticks.map((t) =>
        m('g', [
          m('line', {
            x1: padLeft - TICK_LENGTH,
            y1: yToPx(t),
            x2: padLeft,
            y2: yToPx(t),
            stroke: BORDER_COLOR,
          }),
          m(
            'text',
            {
              'x': padLeft - TICK_LENGTH - TICK_LABEL_GAP,
              'y': yToPx(t),
              'fill': TEXT_COLOR,
              'font-size': AXIS_LABEL_FONT_SIZE,
              'text-anchor': 'end',
              'dominant-baseline': 'middle',
            },
            fmtY(t),
          ),
        ]),
      ),
      // X ticks + labels
      xRange.ticks.map((t) =>
        m('g', [
          m('line', {
            x1: xToPx(t),
            y1: padTop + plotH,
            x2: xToPx(t),
            y2: padTop + plotH + TICK_LENGTH,
            stroke: BORDER_COLOR,
          }),
          m(
            'text',
            {
              'x': xToPx(t),
              'y': padTop + plotH + TICK_LENGTH + TICK_LABEL_GAP,
              'fill': TEXT_COLOR,
              'font-size': AXIS_LABEL_FONT_SIZE,
              'text-anchor': 'middle',
              'dominant-baseline': 'hanging',
            },
            fmtX(t),
          ),
        ]),
      ),
      // Everything below renders in *data* coordinates and is clipped to
      // the plot area so it can't bleed over the axes.
      m('g', {'clip-path': `url(#${this.clipId})`}, [
        // Series areas (stacked) — drawn back-to-front so the topmost
        // series sits on top. The area path is also the hit target for
        // hover emphasis.
        stacked &&
          seriesPlots.map((s, i) => {
            const dim =
              this.hoveredSeries !== undefined && this.hoveredSeries !== i;
            return m('path', {
              'd': areaPath(s, seriesPlots, i, xToPx, yToPx, padTop + plotH),
              'fill': s.color,
              'fill-opacity': dim ? 0.4 : 0.8,
              'stroke': 'none',
              'style': attrs.onSeriesClick && {cursor: 'pointer'},
              'onmouseenter': () => this.setHoveredSeries(i),
              'onmouseleave': () => this.clearHoveredSeries(i),
              'onclick':
                attrs.onSeriesClick && (() => attrs.onSeriesClick!(s.name)),
            });
          }),
        // Series lines (visible stroke).
        seriesPlots.map((s, i) => {
          const dim =
            this.hoveredSeries !== undefined && this.hoveredSeries !== i;
          return m('path', {
            'd': linePath(s, xToPx, yToPx),
            'fill': 'none',
            'stroke': s.color,
            'stroke-width': lineWidth,
            'opacity': dim ? 0.6 : 1,
            'pointer-events': 'none',
          });
        }),
        // Wider invisible hit target per series so hovering near a thin
        // line counts. Drawn after visible lines and before points so it
        // sits above for hit-testing without occluding visuals.
        !stacked &&
          seriesPlots.map((s, i) =>
            m('path', {
              'd': linePath(s, xToPx, yToPx),
              'fill': 'none',
              'stroke': 'transparent',
              'stroke-width': 12,
              'pointer-events': 'stroke',
              'style': attrs.onSeriesClick && {cursor: 'pointer'},
              'onmouseenter': () => this.setHoveredSeries(i),
              'onmouseleave': () => this.clearHoveredSeries(i),
              'onclick':
                attrs.onSeriesClick && (() => attrs.onSeriesClick!(s.name)),
            }),
          ),
        // Series points
        showPoints &&
          seriesPlots.map((s, i) => {
            const dim =
              this.hoveredSeries !== undefined && this.hoveredSeries !== i;
            return m(
              'g',
              {
                'opacity': dim ? 0.2 : 1,
                'pointer-events': 'none',
              },
              s.points.map((p) =>
                pointMarker(xToPx(p.x), yToPx(p.y), s.color, 3),
              ),
            );
          }),
        // Hover guide line + dots. pointer-events: none so they don't
        // steal mouseenter/leave from the per-series hit targets
        // underneath.
        hoverIdx !== undefined &&
          seriesPlots[0]?.points[hoverIdx] !== undefined &&
          m('g', {'pointer-events': 'none'}, [
            m('line', {
              'x1': xToPx(seriesPlots[0].points[hoverIdx].x),
              'y1': padTop,
              'x2': xToPx(seriesPlots[0].points[hoverIdx].x),
              'y2': padTop + plotH,
              'stroke': TEXT_COLOR,
              'stroke-dasharray': '3 3',
            }),
            ...seriesPlots.flatMap((s) => {
              const p = s.points[hoverIdx];
              if (p === undefined) return [];
              return [pointMarker(xToPx(p.x), yToPx(p.y), s.color, 4)];
            }),
          ]),
      ]),
      // Active brush drag rect (drawn on top of everything else).
      this.brushing &&
        (() => {
          const a = clamp(this.brushing!.start, xRange.min, xRange.max);
          const b = clamp(this.brushing!.current, xRange.min, xRange.max);
          const lo = Math.min(a, b);
          const hi = Math.max(a, b);
          return m('rect', {
            'x': xToPx(lo),
            'y': padTop,
            'width': Math.max(0, xToPx(hi) - xToPx(lo)),
            'height': plotH,
            'fill': 'rgba(0, 120, 212, 0.15)',
            'stroke': 'rgba(0, 120, 212, 0.5)',
            'stroke-width': 1,
            'pointer-events': 'none',
          });
        })(),
      // Tooltip (HTML-overlaid via foreignObject would be cleaner, but a
      // sibling absolutely-positioned div is simpler and gets real CSS).
    );
  }

  private handleBrushDown(
    e: PointerEvent,
    padLeft: number,
    plotW: number,
    xRange: AxisRange,
  ) {
    const svg = e.currentTarget as SVGSVGElement;
    const rect = svg.getBoundingClientRect();
    const px = clamp(e.clientX - rect.left, padLeft, padLeft + plotW);
    const xVal =
      xRange.min + ((px - padLeft) / (plotW || 1)) * (xRange.max - xRange.min);
    // Capture the pointer so subsequent move/up events fire on the SVG even
    // if the cursor leaves it. Capture is auto-released on pointerup.
    svg.setPointerCapture(e.pointerId);
    this.brushing = {start: xVal, current: xVal, pointerId: e.pointerId};
    this.hover = undefined;
    e.preventDefault();
  }

  private handleBrushUp(
    e: PointerEvent,
    onBrush: (range: {start: number; end: number}) => void,
  ) {
    if (!this.brushing || this.brushing.pointerId !== e.pointerId) return;
    const {start, current} = this.brushing;
    const lo = Math.min(start, current);
    const hi = Math.max(start, current);
    this.brushing = undefined;
    if (hi > lo) onBrush({start: lo, end: hi});
  }

  private handlePointerMove(
    e: PointerEvent,
    seriesPlots: ReadonlyArray<SeriesPlot>,
    padLeft: number,
    plotW: number,
    xRange: AxisRange,
  ) {
    const svg = e.currentTarget as SVGSVGElement;
    const rect = svg.getBoundingClientRect();
    // While brushing, track the drag end and suppress the cursor tooltip —
    // it gets in the way of seeing what you're selecting.
    if (this.brushing && this.brushing.pointerId === e.pointerId) {
      const px = clamp(e.clientX - rect.left, padLeft, padLeft + plotW);
      this.brushing = {
        start: this.brushing.start,
        current:
          xRange.min +
          ((px - padLeft) / (plotW || 1)) * (xRange.max - xRange.min),
        pointerId: e.pointerId,
      };
      this.hover = undefined;
      return;
    }
    if (seriesPlots.length === 0 || seriesPlots[0].points.length === 0) return;
    const px = e.clientX - rect.left;
    if (px < padLeft || px > padLeft + plotW) {
      if (this.hover !== undefined) {
        this.hover = undefined;
      }
      return;
    }
    const xVal =
      xRange.min + ((px - padLeft) / (plotW || 1)) * (xRange.max - xRange.min);
    // Nearest index in series 0 (assumes shared x; first series is reference).
    const pts = seriesPlots[0].points;
    let bestIdx = 0;
    let bestDist = Infinity;
    for (let i = 0; i < pts.length; i++) {
      const d = Math.abs(pts[i].x - xVal);
      if (d < bestDist) {
        bestDist = d;
        bestIdx = i;
      }
    }
    this.hover = {
      clientX: e.clientX,
      clientY: e.clientY,
      index: bestIdx,
      xValue: pts[bestIdx].x,
    };
  }
}

function buildSeriesPlots(
  series: ReadonlyArray<LineChartSeries>,
  stacked: boolean,
): ReadonlyArray<SeriesPlot> {
  if (!stacked) {
    return series.map((s, i) => ({
      name: s.name,
      color: s.color ?? chartColorVar(i),
      points: s.points.map((p) => ({x: p.x, y: p.y, raw: p.y})),
    }));
  }
  // Stacked: assume aligned x values across series; sum cumulatively.
  const result: SeriesPlot[] = [];
  const cum = new Map<number, number>();
  for (let i = 0; i < series.length; i++) {
    const s = series[i];
    const stackedPoints: {x: number; y: number; raw: number}[] = [];
    for (const p of s.points) {
      const prev = cum.get(p.x) ?? 0;
      const next = prev + p.y;
      cum.set(p.x, next);
      stackedPoints.push({x: p.x, y: next, raw: p.y});
    }
    result.push({
      name: s.name,
      color: s.color ?? chartColorVar(i),
      points: stackedPoints,
    });
  }
  return result;
}

function linePath(
  s: SeriesPlot,
  xToPx: (x: number) => number,
  yToPx: (y: number) => number,
): string {
  if (s.points.length === 0) return '';
  let d = '';
  for (let i = 0; i < s.points.length; i++) {
    const p = s.points[i];
    d += (i === 0 ? 'M' : 'L') + xToPx(p.x) + ',' + yToPx(p.y) + ' ';
  }
  return d.trim();
}

// For stacked areas, the bottom of series i is the top of series i-1
// (in the same x order). For the bottom-most series, bottom = y0 baseline.
function areaPath(
  s: SeriesPlot,
  all: ReadonlyArray<SeriesPlot>,
  index: number,
  xToPx: (x: number) => number,
  yToPx: (y: number) => number,
  baselinePx: number,
): string {
  if (s.points.length === 0) return '';
  const top = s.points;
  const bottom = index > 0 ? all[index - 1].points : undefined;
  let d = '';
  for (let i = 0; i < top.length; i++) {
    d += (i === 0 ? 'M' : 'L') + xToPx(top[i].x) + ',' + yToPx(top[i].y) + ' ';
  }
  if (bottom !== undefined) {
    for (let i = bottom.length - 1; i >= 0; i--) {
      d += 'L' + xToPx(bottom[i].x) + ',' + yToPx(bottom[i].y) + ' ';
    }
  } else {
    d += 'L' + xToPx(top[top.length - 1].x) + ',' + baselinePx + ' ';
    d += 'L' + xToPx(top[0].x) + ',' + baselinePx + ' ';
  }
  d += 'Z';
  return d;
}
