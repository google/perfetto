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
  CHART_COLORS,
  estimateTickCount,
  formatNumber,
  generateLogTicks,
  generateTicks,
  truncateLabel,
} from './chart_utils';
import {SvgBrush} from './svg_brush';

/**
 * A single data point in a line chart series.
 */
export interface LineChartPoint {
  /** X-axis value (typically time or sequential index) */
  readonly x: number;
  /** Y-axis value */
  readonly y: number;
}

/**
 * A single series (line) in the chart.
 */
export interface LineChartSeries {
  /** Display name for this series (shown in legend) */
  readonly name: string;
  /** Data points for this series, sorted by x value */
  readonly points: readonly LineChartPoint[];
  /** Optional custom color for this series */
  readonly color?: string;
}

/**
 * Data provided to a LineChart.
 */
export interface LineChartData {
  /** The series to display */
  readonly series: readonly LineChartSeries[];
}

export interface LineChartAttrs {
  /**
   * Line chart data to display, or undefined if loading.
   * When undefined, a loading spinner is shown.
   */
  readonly data: LineChartData | undefined;

  /**
   * Height of the chart in pixels. Defaults to 200.
   */
  readonly height?: number;

  /**
   * Label for the X axis.
   */
  readonly xAxisLabel?: string;

  /**
   * Label for the Y axis.
   */
  readonly yAxisLabel?: string;

  /**
   * Callback when brush selection completes (on mouseup).
   * Called with the selected X range.
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
   * Use logarithmic scale for Y axis. Defaults to false.
   */
  readonly logScale?: boolean;

  /**
   * When true, X axis ticks will be snapped to integer values.
   */
  readonly integerX?: boolean;

  /**
   * When true, Y axis ticks will be snapped to integer values.
   */
  readonly integerY?: boolean;

  /**
   * Show legend. Defaults to true when multiple series.
   */
  readonly showLegend?: boolean;

  /**
   * Show data points as circles. Defaults to true.
   */
  readonly showPoints?: boolean;

  /**
   * Line width in pixels. Defaults to 2.
   */
  readonly lineWidth?: number;
}

const DEFAULT_HEIGHT = 200;
const VIEWBOX_WIDTH = 400;
const MARGIN = {top: 10, right: 10, bottom: 40, left: 65};
const LEGEND_HEIGHT = 20;

export class LineChart implements m.ClassComponent<LineChartAttrs> {
  private hoveredPoint?: {series: LineChartSeries; point: LineChartPoint};
  private readonly brush = new SvgBrush();

  view({attrs}: m.Vnode<LineChartAttrs>) {
    const {
      data,
      height = DEFAULT_HEIGHT,
      xAxisLabel,
      yAxisLabel,
      onBrush,
      fillParent,
      className,
      formatXValue = (v) => formatNumber(v),
      formatYValue = (v) => formatNumber(v),
      logScale = false,
      integerX = false,
      integerY = false,
      showLegend,
      showPoints = true,
      lineWidth = 2,
    } = attrs;

    if (data === undefined) {
      return m(
        '.pf-line-chart',
        {
          className: classNames(
            fillParent && 'pf-line-chart--fill-parent',
            className,
          ),
          style: {height: `${height}px`},
        },
        m('.pf-line-chart__loading', m(Spinner)),
      );
    }

    if (
      data.series.length === 0 ||
      data.series.every((s) => s.points.length === 0)
    ) {
      return m(
        '.pf-line-chart',
        {
          className: classNames(
            fillParent && 'pf-line-chart--fill-parent',
            className,
          ),
          style: {height: `${height}px`},
        },
        m('.pf-line-chart__empty', 'No data to display'),
      );
    }

    // Determine if legend should be shown
    const displayLegend = showLegend ?? data.series.length > 1;
    const legendOffset = displayLegend ? LEGEND_HEIGHT : 0;

    const chartWidth = VIEWBOX_WIDTH - MARGIN.left - MARGIN.right;
    const chartHeight = height - MARGIN.top - MARGIN.bottom - legendOffset;

    // Compute bounds across all series
    let minX = Infinity;
    let maxX = -Infinity;
    let minY = Infinity;
    let maxY = -Infinity;

    for (const series of data.series) {
      for (const point of series.points) {
        if (point.x < minX) minX = point.x;
        if (point.x > maxX) maxX = point.x;
        if (point.y < minY) minY = point.y;
        if (point.y > maxY) maxY = point.y;
      }
    }

    // Handle edge cases
    if (minX === maxX) {
      minX -= 1;
      maxX += 1;
    }
    if (minY === maxY) {
      minY = 0;
      maxY = maxY === 0 ? 1 : maxY * 1.1;
    }

    // Start Y axis at 0 unless all values are positive and far from 0
    if (minY > 0 && minY < maxY * 0.3) {
      minY = 0;
    }

    // Generate ticks
    const xTickCount = estimateTickCount(chartWidth, minX, maxX, formatXValue);
    const xTicks = generateTicks(minX, maxX, xTickCount, integerX);
    const yTicks = logScale
      ? generateLogTicks(maxY)
      : generateTicks(minY, maxY, 5, integerY);

    // Coordinate converters
    const xToChart = (x: number): number => {
      return ((x - minX) / (maxX - minX)) * chartWidth;
    };

    const yToChart = (y: number): number => {
      if (logScale) {
        if (y <= 0) return chartHeight;
        const logMin = minY > 0 ? Math.log10(minY) : 0;
        const logMax = Math.log10(maxY);
        const logY = Math.log10(y);
        return (
          chartHeight - ((logY - logMin) / (logMax - logMin)) * chartHeight
        );
      }
      return chartHeight - ((y - minY) / (maxY - minY)) * chartHeight;
    };

    // Convert chart X to data value
    const chartXToValue = (chartX: number): number => {
      const ratio = Math.max(0, Math.min(1, chartX / chartWidth));
      return minX + ratio * (maxX - minX);
    };

    // Build path for each series
    const seriesPaths = data.series.map((series, seriesIdx) => {
      const color =
        series.color ?? CHART_COLORS[seriesIdx % CHART_COLORS.length];

      if (series.points.length === 0) return null;

      // Build path string
      const pathParts: string[] = [];
      for (let i = 0; i < series.points.length; i++) {
        const point = series.points[i];
        const cx = xToChart(point.x);
        const cy = yToChart(point.y);
        pathParts.push(i === 0 ? `M${cx},${cy}` : `L${cx},${cy}`);
      }

      return m('g.pf-line-chart__series', [
        // Line
        m('path.pf-line-chart__line', {
          'd': pathParts.join(' '),
          'stroke': color,
          'stroke-width': lineWidth,
        }),
        // Points
        showPoints &&
          series.points.map((point) => {
            const cx = xToChart(point.x);
            const cy = yToChart(point.y);
            const isHovered =
              this.hoveredPoint?.series === series &&
              this.hoveredPoint?.point === point;
            return m('circle.pf-line-chart__point', {
              'cx': cx,
              'cy': cy,
              'r': isHovered ? 5 : 3,
              'fill': color,
              'stroke': 'var(--pf-color-background)',
              'stroke-width': 1,
              'onmouseenter': () => {
                this.hoveredPoint = {series, point};
              },
              'onmouseleave': () => {
                this.hoveredPoint = undefined;
              },
            });
          }),
      ]);
    });

    const style: Record<string, string> = {height: `${height}px`};

    return m(
      '.pf-line-chart',
      {
        className: classNames(
          fillParent && 'pf-line-chart--fill-parent',
          className,
        ),
        style,
      },
      [
        m(
          'svg.pf-line-chart__svg',
          {
            viewBox: `0 0 ${VIEWBOX_WIDTH} ${height}`,
            preserveAspectRatio: 'xMidYMid meet',
          },
          [
            // Chart area
            m(
              'g.pf-line-chart__chart-area',
              {
                transform: `translate(${MARGIN.left}, ${MARGIN.top})`,
                ...(onBrush
                  ? this.brush.chartAreaAttrs(
                      {left: MARGIN.left, top: MARGIN.top},
                      'horizontal',
                      (startX, endX) => {
                        onBrush({
                          start: chartXToValue(startX),
                          end: chartXToValue(endX),
                        });
                      },
                    )
                  : {}),
              },
              [
                // Background
                m('rect.pf-line-chart__background', {
                  x: 0,
                  y: 0,
                  width: chartWidth,
                  height: chartHeight,
                  fill: 'transparent',
                }),

                // Series lines and points
                ...seriesPaths,

                // Brush selection
                this.brush.renderSelection(
                  chartWidth,
                  chartHeight,
                  'horizontal',
                  'pf-line-chart__brush-selection',
                ),

                // X Axis
                m(
                  'g.pf-line-chart__x-axis',
                  {transform: `translate(0, ${chartHeight})`},
                  [
                    m('line.pf-line-chart__axis-line', {
                      x1: 0,
                      y1: 0,
                      x2: chartWidth,
                      y2: 0,
                    }),
                    ...xTicks.map((tick) => {
                      const x = xToChart(tick);
                      return m('g', {transform: `translate(${x}, 0)`}, [
                        m('line.pf-line-chart__tick', {y2: 5}),
                        m(
                          'text.pf-line-chart__tick-label',
                          {
                            'y': 15,
                            'text-anchor': 'middle',
                            'dominant-baseline': 'middle',
                          },
                          formatXValue(tick),
                        ),
                      ]);
                    }),
                    xAxisLabel &&
                      m(
                        'text.pf-line-chart__axis-label',
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
                m('g.pf-line-chart__y-axis', [
                  m('line.pf-line-chart__axis-line', {
                    x1: 0,
                    y1: 0,
                    x2: 0,
                    y2: chartHeight,
                  }),
                  ...yTicks.map((tick) => {
                    const y = yToChart(tick);
                    return m('g', {transform: `translate(0, ${y})`}, [
                      m('line.pf-line-chart__tick', {x2: -5}),
                      m(
                        'text.pf-line-chart__tick-label',
                        {
                          'x': -8,
                          'text-anchor': 'end',
                          'dominant-baseline': 'middle',
                        },
                        formatYValue(tick),
                      ),
                    ]);
                  }),
                  yAxisLabel &&
                    m(
                      'text.pf-line-chart__axis-label',
                      {
                        'transform': `translate(-50, ${chartHeight / 2}) rotate(-90)`,
                        'text-anchor': 'middle',
                      },
                      yAxisLabel,
                    ),
                ]),
              ],
            ),

            // Legend
            displayLegend &&
              m(
                'g.pf-line-chart__legend',
                {
                  transform: `translate(${MARGIN.left}, ${height - LEGEND_HEIGHT + 5})`,
                },
                (() => {
                  const SWATCH_WIDTH = 15;
                  const GAP = 8;
                  const TEXT_OFFSET = SWATCH_WIDTH + 5;
                  const CHAR_WIDTH = 6;
                  const MAX_LABEL_CHARS = 10;
                  let xOffset = 0;
                  return data.series.map((series, idx) => {
                    const color =
                      series.color ?? CHART_COLORS[idx % CHART_COLORS.length];
                    const label = truncateLabel(series.name, MAX_LABEL_CHARS);
                    const itemX = xOffset;
                    xOffset += TEXT_OFFSET + label.length * CHAR_WIDTH + GAP;
                    return m('g', {transform: `translate(${itemX}, 0)`}, [
                      m('line', {
                        'x1': 0,
                        'y1': 5,
                        'x2': SWATCH_WIDTH,
                        'y2': 5,
                        'stroke': color,
                        'stroke-width': 2,
                      }),
                      m(
                        'text.pf-line-chart__legend-label',
                        {
                          'x': TEXT_OFFSET,
                          'y': 5,
                          'dominant-baseline': 'middle',
                        },
                        label,
                      ),
                    ]);
                  });
                })(),
              ),
          ],
        ),
        // Tooltip
        this.hoveredPoint &&
          m(
            '.pf-line-chart__tooltip',
            m('.pf-line-chart__tooltip-content', [
              m(
                '.pf-line-chart__tooltip-row',
                `${this.hoveredPoint.series.name}`,
              ),
              m(
                '.pf-line-chart__tooltip-row',
                `X: ${formatXValue(this.hoveredPoint.point.x)}`,
              ),
              m(
                '.pf-line-chart__tooltip-row',
                `Y: ${formatYValue(this.hoveredPoint.point.y)}`,
              ),
            ]),
          ),
      ],
    );
  }
}
