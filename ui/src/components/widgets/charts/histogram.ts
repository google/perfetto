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
import {classNames} from '../../../base/classnames';
import {Spinner} from '../../../widgets/spinner';
import {
  HistogramBucket,
  HistogramData,
  HistogramConfig,
  computeHistogram,
} from './histogram_loader';

// Re-export data types for convenience
export {HistogramBucket, HistogramData, HistogramConfig, computeHistogram};

export interface HistogramAttrs {
  /**
   * Histogram data to display, or undefined if loading.
   * When undefined, a loading spinner is shown.
   * Use the computeHistogram() utility function to compute this from raw values.
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
   * Bar color. Defaults to CSS variable.
   */
  readonly barColor?: string;

  /**
   * Bar hover color. Defaults to CSS variable.
   */
  readonly barHoverColor?: string;

  /**
   * Use logarithmic scale for Y axis. Useful when count values
   * span multiple orders of magnitude. Defaults to false.
   */
  readonly logScale?: boolean;
}

const DEFAULT_HEIGHT = 200;
const VIEWBOX_WIDTH = 400;
const MARGIN = {top: 10, right: 10, bottom: 40, left: 50};

export class Histogram implements m.ClassComponent<HistogramAttrs> {
  private hoveredBucket?: HistogramBucket;
  private brushStart?: number;
  private brushEnd?: number;

  view({attrs}: m.Vnode<HistogramAttrs>) {
    const {
      data,
      height = DEFAULT_HEIGHT,
      xAxisLabel,
      yAxisLabel = 'Count',
      onBrush,
      fillParent,
      className,
      formatXValue = (v) => formatNumber(v),
      formatYValue = (v) => formatNumber(v),
      barColor,
      barHoverColor,
      logScale = false,
    } = attrs;

    if (data === undefined) {
      return m(
        '.pf-histogram',
        {
          className: classNames(
            fillParent && 'pf-histogram--fill-parent',
            className,
          ),
          style: {height: `${height}px`},
        },
        m('.pf-histogram__loading', m(Spinner)),
      );
    }

    if (data.buckets.length === 0) {
      return m(
        '.pf-histogram',
        {
          className: classNames(
            fillParent && 'pf-histogram--fill-parent',
            className,
          ),
          style: {height: `${height}px`},
        },
        m('.pf-histogram__empty', 'No data to display'),
      );
    }

    const chartWidth = VIEWBOX_WIDTH - MARGIN.left - MARGIN.right;
    const chartHeight = height - MARGIN.top - MARGIN.bottom;

    const maxCount = Math.max(
      ...data.buckets.map((b: HistogramBucket) => b.count),
    );
    const bucketWidth = chartWidth / data.buckets.length;

    // Generate Y axis ticks
    const yTicks = logScale
      ? generateLogTicks(maxCount)
      : generateTicks(0, maxCount, 5);

    // Helper to convert count value to Y position
    const countToY = (count: number): number => {
      if (logScale) {
        if (count <= 0) return chartHeight;
        return (
          chartHeight - (Math.log10(count) / Math.log10(maxCount)) * chartHeight
        );
      }
      return chartHeight - (count / maxCount) * chartHeight;
    };

    // Generate X axis ticks (show min, max, and a few intermediate values)
    const xTicks = generateTicks(data.min, data.max, 5);

    const style: Record<string, string> = {height: `${height}px`};
    if (barColor) style['--pf-histogram-bar-color'] = barColor;
    if (barHoverColor) style['--pf-histogram-bar-hover-color'] = barHoverColor;

    // Helper to convert client X coordinate to data value
    // Uses SVG's coordinate transformation to handle viewBox and preserveAspectRatio
    const clientXToValue = (e: PointerEvent): number => {
      const group = e.currentTarget as SVGGElement;
      const svg = group.ownerSVGElement!;
      // Create a point in screen coordinates
      const point = svg.createSVGPoint();
      point.x = e.clientX;
      point.y = e.clientY;
      // Transform to SVG viewBox coordinates
      const ctm = svg.getScreenCTM();
      if (!ctm) return data.min;
      const svgPoint = point.matrixTransform(ctm.inverse());
      // Subtract margin to get chart-relative X
      const chartX = svgPoint.x - MARGIN.left;
      // Clamp to chart bounds and convert to data value
      const ratio = Math.max(0, Math.min(1, chartX / chartWidth));
      return data.min + ratio * (data.max - data.min);
    };

    return m(
      '.pf-histogram',
      {
        className: classNames(
          fillParent && 'pf-histogram--fill-parent',
          className,
        ),
        style,
      },
      m(
        'svg.pf-histogram__svg',
        {
          viewBox: `0 0 ${VIEWBOX_WIDTH} ${height}`,
          preserveAspectRatio: 'xMidYMid meet',
        },
        [
          // Chart area group with margins and brush event handlers
          m(
            'g.pf-histogram__chart-area',
            {
              transform: `translate(${MARGIN.left}, ${MARGIN.top})`,
              style: onBrush ? {cursor: 'col-resize'} : undefined,
              onpointerdown: onBrush
                ? (e: PointerEvent) => {
                    (e.currentTarget as Element).setPointerCapture(e.pointerId);
                    this.brushStart = clientXToValue(e);
                    this.brushEnd = this.brushStart;
                  }
                : undefined,
              onpointermove: onBrush
                ? (e: PointerEvent) => {
                    if (this.brushStart === undefined) return;
                    this.brushEnd = clientXToValue(e);
                  }
                : undefined,
              onpointerup: onBrush
                ? (e: PointerEvent) => {
                    (e.currentTarget as Element).releasePointerCapture(
                      e.pointerId,
                    );
                    if (
                      this.brushStart !== undefined &&
                      this.brushEnd !== undefined
                    ) {
                      const start = Math.min(this.brushStart, this.brushEnd);
                      const end = Math.max(this.brushStart, this.brushEnd);
                      onBrush({start, end});
                    }
                    this.brushStart = undefined;
                    this.brushEnd = undefined;
                  }
                : undefined,
            },
            [
              // Background rect to catch clicks in gaps between bars
              m('rect.pf-histogram__background', {
                x: 0,
                y: 0,
                width: chartWidth,
                height: chartHeight,
                fill: 'transparent',
              }),

              // Bars (hover events work directly, mousedown bubbles to parent)
              data.buckets.map((bucket: HistogramBucket, i: number) => {
                // Skip zero-count bars in log scale (log(0) is undefined)
                if (logScale && bucket.count === 0) return null;

                const y = countToY(bucket.count);
                const barHeight = chartHeight - y;
                const x = i * bucketWidth;
                const isHovered = this.hoveredBucket === bucket;

                return m('rect.pf-histogram__bar', {
                  x,
                  y,
                  width: Math.max(bucketWidth - 1, 1),
                  height: barHeight,
                  className: classNames(
                    isHovered && 'pf-histogram__bar--hover',
                  ),
                  onmouseenter: () => {
                    this.hoveredBucket = bucket;
                  },
                  onmouseleave: () => {
                    this.hoveredBucket = undefined;
                  },
                });
              }),

              // Brush selection rectangle (visual only, no pointer events)
              this.brushStart !== undefined &&
                this.brushEnd !== undefined &&
                (() => {
                  const startX =
                    ((Math.min(this.brushStart, this.brushEnd) - data.min) /
                      (data.max - data.min)) *
                    chartWidth;
                  const endX =
                    ((Math.max(this.brushStart, this.brushEnd) - data.min) /
                      (data.max - data.min)) *
                    chartWidth;
                  return m('rect.pf-histogram__brush-selection', {
                    x: startX,
                    y: 0,
                    width: Math.max(0, endX - startX),
                    height: chartHeight,
                  });
                })(),

              // X Axis
              m(
                'g.pf-histogram__x-axis',
                {transform: `translate(0, ${chartHeight})`},
                [
                  // Axis line
                  m('line.pf-histogram__axis-line', {
                    x1: 0,
                    y1: 0,
                    x2: chartWidth,
                    y2: 0,
                  }),
                  // Ticks
                  ...xTicks.map((tick) => {
                    const x =
                      ((tick - data.min) / (data.max - data.min)) * chartWidth;
                    return m('g', {transform: `translate(${x}, 0)`}, [
                      m('line.pf-histogram__tick', {y2: 5}),
                      m(
                        'text.pf-histogram__tick-label',
                        {
                          'y': 15,
                          'text-anchor': 'middle',
                          'dominant-baseline': 'middle',
                        },
                        formatXValue(tick),
                      ),
                    ]);
                  }),
                  // Axis label
                  xAxisLabel &&
                    m(
                      'text.pf-histogram__axis-label',
                      {
                        'x': chartWidth / 2,
                        'y': 30,
                        'text-anchor': 'middle',
                      },
                      xAxisLabel,
                    ),
                ],
              ),

              // Y Axis
              m('g.pf-histogram__y-axis', [
                // Axis line
                m('line.pf-histogram__axis-line', {
                  x1: 0,
                  y1: 0,
                  x2: 0,
                  y2: chartHeight,
                }),
                // Ticks
                ...yTicks.map((tick) => {
                  const y = countToY(tick);
                  return m('g', {transform: `translate(0, ${y})`}, [
                    m('line.pf-histogram__tick', {x2: -5}),
                    m(
                      'text.pf-histogram__tick-label',
                      {
                        'x': -8,
                        'text-anchor': 'end',
                        'dominant-baseline': 'middle',
                      },
                      formatYValue(tick),
                    ),
                  ]);
                }),
                // Axis label
                yAxisLabel &&
                  m(
                    'text.pf-histogram__axis-label',
                    {
                      'transform': `translate(-35, ${chartHeight / 2}) rotate(-90)`,
                      'text-anchor': 'middle',
                    },
                    yAxisLabel,
                  ),
              ]),
            ],
          ),
        ],
      ),
      // Tooltip
      this.hoveredBucket &&
        m(
          '.pf-histogram__tooltip',
          m('.pf-histogram__tooltip-content', [
            m(
              '.pf-histogram__tooltip-row',
              `Range: ${formatXValue(this.hoveredBucket.start)} - ${formatXValue(this.hoveredBucket.end)}`,
            ),
            m(
              '.pf-histogram__tooltip-row',
              `Count: ${formatYValue(this.hoveredBucket.count)}`,
            ),
            data.totalCount > 0 &&
              m(
                '.pf-histogram__tooltip-row',
                `${((this.hoveredBucket.count / data.totalCount) * 100).toFixed(1)}%`,
              ),
          ]),
        ),
    );
  }
}

/**
 * Generate nice tick values for an axis.
 */
function generateTicks(min: number, max: number, count: number): number[] {
  if (min === max) return [min];

  const range = max - min;
  const step = range / (count - 1);
  const ticks: number[] = [];

  for (let i = 0; i < count; i++) {
    ticks.push(min + i * step);
  }

  return ticks;
}

/**
 * Format a number for display.
 */
function formatNumber(value: number): string {
  if (Number.isInteger(value)) {
    return value.toLocaleString();
  }
  // For decimals, show up to 2 decimal places
  if (Math.abs(value) >= 1000) {
    return value.toLocaleString(undefined, {maximumFractionDigits: 0});
  }
  if (Math.abs(value) >= 1) {
    return value.toLocaleString(undefined, {maximumFractionDigits: 2});
  }
  // For very small numbers, use more precision
  return value.toPrecision(3);
}

/**
 * Generate tick values for a logarithmic scale (powers of 10).
 */
function generateLogTicks(max: number): number[] {
  if (max <= 1) return [1];
  const ticks: number[] = [1];
  let power = 1;
  while (Math.pow(10, power) <= max) {
    ticks.push(Math.pow(10, power));
    power++;
  }
  return ticks;
}
