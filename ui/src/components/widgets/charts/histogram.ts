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
import {formatNumber} from './chart_utils';
import {
  HistogramBucket,
  HistogramData,
  HistogramConfig,
  computeHistogram,
} from './histogram_loader';
import {SELECTION_COLOR} from './chart_option_builder';
import {getChartThemeColors} from './chart_theme';

// Re-export data types for convenience
export {HistogramBucket, HistogramData, HistogramConfig, computeHistogram};

const SELECTION_BG_COLOR = 'rgba(0, 120, 212, 0.08)';
const DEFAULT_HEIGHT = 200;

// Margins around the plot area (space for axis labels/ticks).
const MARGIN = {top: 10, right: 10, bottom: 30, left: 50};

export interface HistogramAttrs {
  /**
   * Histogram data to display, or undefined if loading.
   * When undefined, a loading spinner is shown.
   * Use the computeHistogram() utility function to compute this from raw
   * values.
   */
  readonly data: HistogramData | undefined;

  /**
   * Height of the histogram in pixels. Defaults to 200.
   */
  readonly height?: number;

  /**
   * Label for the X axis.
   */
  readonly xAxisLabel?: string;

  /**
   * Label for the Y axis. Defaults to 'Count'.
   */
  readonly yAxisLabel?: string;

  /**
   * Callback when brush selection completes (on mouseup).
   * Called with the selected range based on mousedown and mouseup positions.
   */
  readonly onBrush?: (range: {start: number; end: number}) => void;

  /**
   * Selection range to highlight on the chart. Buckets overlapping this
   * range are drawn with a highlight color. The consumer controls this
   * state — typically by feeding the `onBrush` output back in.
   */
  readonly selection?: {readonly start: number; readonly end: number};

  /**
   * Fill parent container. Defaults to false.
   */
  readonly fillParent?: boolean;

  /**
   * Custom class name for the container.
   */
  readonly className?: string;

  /**
   * Format function for X axis tick values.
   */
  readonly formatXValue?: (value: number) => string;

  /**
   * Format function for Y axis tick values.
   */
  readonly formatYValue?: (value: number) => string;

  /**
   * Bar color. Defaults to theme primary color.
   */
  readonly barColor?: string;

  /**
   * Bar hover color. Defaults to theme accent color.
   */
  readonly barHoverColor?: string;

  /**
   * Use logarithmic scale for Y axis. Useful when count values
   * span multiple orders of magnitude. Defaults to false.
   */
  readonly logScale?: boolean;

  /**
   * When true, X axis (dimension) ticks will be snapped to integer values.
   * Use when the histogram data represents integer-valued quantities.
   * The Y axis (measure) always uses integer ticks since it shows counts.
   */
  readonly integerDimension?: boolean;
}

// Compute ~5 nice tick values for an axis range.
function computeTicks(min: number, max: number, logScale: boolean): number[] {
  if (logScale) {
    if (max <= 0) return [1];
    const minLog = 0; // Always start at 10^0 = 1
    const maxLog = Math.ceil(Math.log10(max));
    const ticks: number[] = [];
    for (let i = minLog; i <= maxLog; i++) {
      ticks.push(Math.pow(10, i));
    }
    return ticks;
  }

  const range = max - min;
  if (range === 0) return [min];
  const rawStep = range / 5;
  const mag = Math.pow(10, Math.floor(Math.log10(rawStep)));
  const ratio = rawStep / mag;
  let step: number;
  if (ratio < 1.5) step = mag;
  else if (ratio < 3.5) step = 2 * mag;
  else if (ratio < 7.5) step = 5 * mag;
  else step = 10 * mag;

  const ticks: number[] = [];
  const start = Math.ceil(min / step) * step;
  for (let v = start; v <= max + step * 0.01; v += step) {
    ticks.push(v);
  }
  return ticks;
}

export class Histogram implements m.ClassComponent<HistogramAttrs> {
  private hoverIdx = -1;
  private tooltip?: {x: number; y: number; html: string};
  private containerWidth = 0;
  private resizeObs?: ResizeObserver;
  private dom?: Element;

  oncreate({dom}: m.CVnodeDOM<HistogramAttrs>) {
    this.dom = dom;
    this.resizeObs = new ResizeObserver(() => {
      const w = (dom as HTMLElement).clientWidth;
      if (w !== this.containerWidth) {
        this.containerWidth = w;
        m.redraw();
      }
    });
    this.resizeObs.observe(dom as HTMLElement);
    this.containerWidth = (dom as HTMLElement).clientWidth;
  }

  onremove() {
    this.resizeObs?.disconnect();
  }

  view({attrs}: m.Vnode<HistogramAttrs>) {
    const {data, height = DEFAULT_HEIGHT, className, fillParent} = attrs;
    const formatXValue = attrs.formatXValue ?? formatNumber;
    const formatYValue = attrs.formatYValue ?? formatNumber;
    const logScale = attrs.logScale ?? false;
    const sel = attrs.selection;

    const hasData =
      data !== undefined &&
      (data.buckets.length > 0 || (data.nullCount ?? 0) > 0);

    if (data === undefined) {
      return m(
        'div',
        {
          class: className,
          style: {
            height: `${height}px`,
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            ...(fillParent ? {width: '100%'} : {}),
          },
        },
        'Loading...',
      );
    }

    if (!hasData) {
      return m(
        'div',
        {
          class: className,
          style: {
            height: `${height}px`,
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            ...(fillParent ? {width: '100%'} : {}),
          },
        },
        'No data',
      );
    }

    // Read theme colors from DOM if available.
    const colors =
      this.dom !== undefined ? getChartThemeColors(this.dom) : undefined;
    const barColor = attrs.barColor ?? colors?.chartColors[0] ?? '#6e93d6';
    const textColor = colors?.textColor ?? '#999';

    const svgWidth = fillParent ? this.containerWidth || 400 : 400;
    const svgHeight = height;
    const plotW = svgWidth - MARGIN.left - MARGIN.right;
    const plotH = svgHeight - MARGIN.top - MARGIN.bottom;

    const buckets = data.buckets;
    const xMin = buckets[0].start;
    const xMax = buckets[buckets.length - 1].end;
    const xRange = xMax - xMin || 1;
    const maxCount = Math.max(...buckets.map((b) => b.count), 1);

    // Scale functions: data -> pixel
    const xScale = (v: number) => MARGIN.left + ((v - xMin) / xRange) * plotW;
    // For log scale, use 0.5 as the floor so bars with count=1 are visible.
    const logMin = 0.5;
    const yScale = (v: number): number => {
      if (logScale) {
        if (v <= logMin) return MARGIN.top + plotH;
        const logRange = Math.log10(maxCount) - Math.log10(logMin);
        const logV = Math.log10(v) - Math.log10(logMin);
        return MARGIN.top + plotH - (logV / logRange) * plotH;
      }
      return MARGIN.top + plotH - (v / maxCount) * plotH;
    };

    const totalWithNull = data.totalCount + (data.nullCount ?? 0);

    // Build bar rects
    const bars: m.Children[] = [];
    for (let i = 0; i < buckets.length; i++) {
      const b = buckets[i];
      const x = Math.floor(xScale(b.start));
      const x2 = Math.floor(xScale(b.end));
      const w = x2 - x;
      const y = yScale(b.count);
      const h = yScale(0) - y;

      // Selection highlight
      const eps = (b.end - b.start) * 0.01;
      const inSelection =
        sel !== undefined && b.end > sel.start + eps && b.start < sel.end - eps;

      if (inSelection) {
        bars.push(
          m('rect', {
            x,
            y: MARGIN.top,
            width: w,
            height: plotH,
            fill: SELECTION_BG_COLOR,
          }),
        );
      }

      const fill = inSelection
        ? SELECTION_COLOR
        : this.hoverIdx === i
          ? attrs.barHoverColor ?? barColor
          : barColor;

      bars.push(
        m('rect', {
          x,
          y: b.count === 0 ? yScale(0) : y,
          width: w,
          height: b.count === 0 ? 0 : Math.max(h, 0),
          fill,
          onmouseenter: (e: MouseEvent) => {
            this.hoverIdx = i;
            const pct =
              totalWithNull > 0
                ? ((b.count / totalWithNull) * 100).toFixed(1)
                : '0';
            this.tooltip = {
              x: e.offsetX,
              y: e.offsetY,
              html: [
                `Range: ${formatXValue(b.start)} \u2013 ${formatXValue(b.end)}`,
                `Count: ${formatYValue(b.count)}`,
                `${pct}%`,
              ].join('\n'),
            };
            m.redraw();
          },
          onmouseleave: () => {
            this.hoverIdx = -1;
            this.tooltip = undefined;
            m.redraw();
          },
        }),
      );
    }

    // Axes
    const xTicks = computeTicks(xMin, xMax, false);
    const yTicks = computeTicks(0, maxCount, logScale);

    const xAxisEls: m.Children[] = [];
    // Axis line
    xAxisEls.push(
      m('line', {
        'x1': MARGIN.left,
        'y1': MARGIN.top + plotH,
        'x2': MARGIN.left + plotW,
        'y2': MARGIN.top + plotH,
        'stroke': textColor,
        'stroke-width': 1,
      }),
    );
    for (const v of xTicks) {
      const x = xScale(v);
      if (x < MARGIN.left - 1 || x > MARGIN.left + plotW + 1) continue;
      xAxisEls.push(
        m('line', {
          'x1': x,
          'y1': MARGIN.top + plotH,
          'x2': x,
          'y2': MARGIN.top + plotH + 4,
          'stroke': textColor,
          'stroke-width': 1,
        }),
      );
      xAxisEls.push(
        m(
          'text',
          {
            x,
            'y': MARGIN.top + plotH + 16,
            'text-anchor': 'middle',
            'fill': textColor,
            'font-size': 10,
          },
          formatXValue(v),
        ),
      );
    }

    const yAxisEls: m.Children[] = [];
    yAxisEls.push(
      m('line', {
        'x1': MARGIN.left,
        'y1': MARGIN.top,
        'x2': MARGIN.left,
        'y2': MARGIN.top + plotH,
        'stroke': textColor,
        'stroke-width': 1,
      }),
    );
    for (const v of yTicks) {
      const y = yScale(v);
      if (y < MARGIN.top - 1 || y > MARGIN.top + plotH + 1) continue;
      yAxisEls.push(
        m('line', {
          'x1': MARGIN.left - 4,
          'y1': y,
          'x2': MARGIN.left,
          'y2': y,
          'stroke': textColor,
          'stroke-width': 1,
        }),
      );
      yAxisEls.push(
        m(
          'text',
          {
            'x': MARGIN.left - 6,
            'y': y + 3,
            'text-anchor': 'end',
            'fill': textColor,
            'font-size': 10,
          },
          formatYValue(v),
        ),
      );
    }

    // Axis labels
    if (attrs.yAxisLabel) {
      yAxisEls.push(
        m(
          'text',
          {
            'x': 12,
            'y': MARGIN.top - 2,
            'fill': textColor,
            'font-size': 11,
            'text-anchor': 'start',
          },
          attrs.yAxisLabel ?? 'Count',
        ),
      );
    }
    if (attrs.xAxisLabel) {
      xAxisEls.push(
        m(
          'text',
          {
            'x': MARGIN.left + plotW / 2,
            'y': svgHeight - 2,
            'fill': textColor,
            'font-size': 11,
            'text-anchor': 'middle',
          },
          attrs.xAxisLabel,
        ),
      );
    }

    return m(
      'div',
      {
        class: className,
        style: {
          position: 'relative',
          ...(fillParent ? {width: '100%'} : {}),
        },
      },
      m(
        'svg',
        {
          width: svgWidth,
          height: svgHeight,
          style: {display: 'block'},
        },
        ...bars,
        ...xAxisEls,
        ...yAxisEls,
      ),
      this.tooltip
        ? m(
            'div',
            {
              style: {
                position: 'absolute',
                left: `${this.tooltip.x + 10}px`,
                top: `${this.tooltip.y - 10}px`,
                background: 'rgba(0, 0, 0, 0.8)',
                color: '#fff',
                padding: '4px 8px',
                borderRadius: '4px',
                fontSize: '11px',
                pointerEvents: 'none',
                whiteSpace: 'pre',
                zIndex: 10,
              },
            },
            this.tooltip.html,
          )
        : null,
    );
  }
}
