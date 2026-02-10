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
  AggregationType,
  formatNumber,
  generateLogTicks,
  generateTicks,
  truncateLabel,
} from './chart_utils';
import {BrushDirection, SvgBrush} from './svg_brush';

/**
 * A single bar in the bar chart.
 */
export interface BarChartItem {
  /** Label for this bar (displayed on the dimension axis). */
  readonly label: string | number;
  /** Numeric value for this bar. */
  readonly value: number;
}

/**
 * Data provided to a BarChart.
 */
export interface BarChartData {
  /** The bars to display. */
  readonly items: readonly BarChartItem[];
}

export interface BarChartAttrs {
  /**
   * Bar chart data to display, or undefined if loading.
   * When undefined, a loading spinner is shown.
   */
  readonly data: BarChartData | undefined;

  /**
   * Height of the chart in pixels. Defaults to 200.
   */
  readonly height?: number;

  /**
   * Label for the dimension axis (the categorical/label axis).
   * Placed on the X axis in vertical mode, Y axis in horizontal mode.
   */
  readonly dimensionLabel?: string;

  /**
   * Label for the measure axis (the numeric value axis).
   * Placed on the Y axis in vertical mode, X axis in horizontal mode.
   */
  readonly measureLabel?: string;

  /**
   * Fill parent container. Defaults to false.
   */
  readonly fillParent?: boolean;

  /**
   * Custom class name for the container.
   */
  readonly className?: string;

  /**
   * Format function for measure axis tick values.
   */
  readonly formatMeasure?: (value: number) => string;

  /**
   * Bar color. Defaults to CSS variable.
   */
  readonly barColor?: string;

  /**
   * Bar hover color. Defaults to CSS variable.
   */
  readonly barHoverColor?: string;

  /**
   * Use logarithmic scale for the measure axis. Defaults to false.
   */
  readonly logScale?: boolean;

  /**
   * When true, measure axis ticks will be snapped to integer values.
   */
  readonly integerMeasure?: boolean;

  /**
   * Chart orientation. Defaults to 'vertical'.
   * - 'vertical': bars grow upward, dimension on X axis, measure on Y axis.
   * - 'horizontal': bars grow rightward, dimension on Y axis, measure on X.
   */
  readonly orientation?: 'vertical' | 'horizontal';

  /**
   * Callback when brush selection completes (on mouseup).
   * Called with the labels of all bars in the brushed range.
   */
  readonly onBrush?: (labels: Array<string | number>) => void;
}

const DEFAULT_HEIGHT = 200;
const VIEWBOX_WIDTH = 400;
const MARGIN_VERTICAL = {top: 10, right: 10, bottom: 50, left: 65};
const MARGIN_HORIZONTAL = {top: 10, right: 10, bottom: 40, left: 80};

export class BarChart implements m.ClassComponent<BarChartAttrs> {
  private hoveredItem?: BarChartItem;
  private readonly brush = new SvgBrush();

  view({attrs}: m.Vnode<BarChartAttrs>) {
    const {
      data,
      height = DEFAULT_HEIGHT,
      dimensionLabel,
      measureLabel = 'Value',
      fillParent,
      className,
      formatMeasure = (v) => formatNumber(v),
      barColor,
      barHoverColor,
      logScale = false,
      integerMeasure = false,
      orientation = 'vertical',
      onBrush,
    } = attrs;

    const horizontal = orientation === 'horizontal';
    const margin = horizontal ? MARGIN_HORIZONTAL : MARGIN_VERTICAL;

    if (data === undefined) {
      return m(
        '.pf-bar-chart',
        {
          className: classNames(
            fillParent && 'pf-bar-chart--fill-parent',
            className,
          ),
          style: {height: `${height}px`},
        },
        m('.pf-bar-chart__loading', m(Spinner)),
      );
    }

    if (data.items.length === 0) {
      return m(
        '.pf-bar-chart',
        {
          className: classNames(
            fillParent && 'pf-bar-chart--fill-parent',
            className,
          ),
          style: {height: `${height}px`},
        },
        m('.pf-bar-chart__empty', 'No data to display'),
      );
    }

    const chartWidth = VIEWBOX_WIDTH - margin.left - margin.right;
    const chartHeight = height - margin.top - margin.bottom;

    const maxValue = Math.max(1, ...data.items.map((item) => item.value));

    // Slot size is the space each bar gets along the dimension axis
    const slotSize = horizontal
      ? chartHeight / data.items.length
      : chartWidth / data.items.length;
    const barPadding = Math.max(slotSize * 0.1, 1);

    // Generate measure axis ticks
    const measureTicks = logScale
      ? generateLogTicks(maxValue)
      : generateTicks(0, maxValue, 5, integerMeasure);

    // Convert value to pixel position on the measure axis
    const valueToMeasure = (value: number): number => {
      if (logScale) {
        if (value <= 0) return 0;
        const ratio = Math.log10(value) / Math.log10(maxValue);
        return horizontal ? ratio * chartWidth : chartHeight * (1 - ratio);
      }
      const ratio = value / maxValue;
      return horizontal ? ratio * chartWidth : chartHeight * (1 - ratio);
    };

    // Format a label for display
    const formatLabel = (label: string | number): string =>
      typeof label === 'number' ? formatNumber(label) : label;

    const style: Record<string, string> = {height: `${height}px`};
    if (barColor) style['--pf-bar-chart-bar-color'] = barColor;
    if (barHoverColor) style['--pf-bar-chart-bar-hover-color'] = barHoverColor;

    // Convert a pixel range on the dimension axis to overlapping bar labels
    const rangeToLabels = (
      start: number,
      end: number,
    ): Array<string | number> => {
      const labels: Array<string | number> = [];
      for (let i = 0; i < data.items.length; i++) {
        const slotStart = i * slotSize;
        const slotEnd = (i + 1) * slotSize;
        if (slotEnd > start && slotStart < end) {
          labels.push(data.items[i].label);
        }
      }
      return labels;
    };

    // Brush direction: across the dimension axis to select bars
    const brushDir: BrushDirection = horizontal ? 'vertical' : 'horizontal';

    return m(
      '.pf-bar-chart',
      {
        className: classNames(
          fillParent && 'pf-bar-chart--fill-parent',
          className,
        ),
        style,
      },
      m(
        'svg.pf-bar-chart__svg',
        {
          viewBox: `0 0 ${VIEWBOX_WIDTH} ${height}`,
          preserveAspectRatio: 'xMidYMid meet',
        },
        [
          m(
            'g.pf-bar-chart__chart-area',
            {
              transform: `translate(${margin.left}, ${margin.top})`,
              ...(onBrush
                ? this.brush.chartAreaAttrs(
                    {left: margin.left, top: margin.top},
                    brushDir,
                    (start, end) => {
                      const labels = rangeToLabels(start, end);
                      if (labels.length > 0) {
                        onBrush(labels);
                      }
                    },
                  )
                : {}),
            },
            [
              // Background
              m('rect.pf-bar-chart__background', {
                x: 0,
                y: 0,
                width: chartWidth,
                height: chartHeight,
                fill: 'transparent',
              }),

              // Bars
              ...data.items.map((item, i) => {
                if (logScale && item.value === 0) return null;
                const isHovered = this.hoveredItem === item;

                if (horizontal) {
                  const barW = valueToMeasure(item.value);
                  const barY = i * slotSize + barPadding;
                  const barH = slotSize - barPadding * 2;
                  return m('rect.pf-bar-chart__bar', {
                    x: 0,
                    y: barY,
                    width: Math.max(barW, 1),
                    height: Math.max(barH, 1),
                    className: classNames(
                      isHovered && 'pf-bar-chart__bar--hover',
                    ),
                    onmouseenter: () => {
                      this.hoveredItem = item;
                    },
                    onmouseleave: () => {
                      this.hoveredItem = undefined;
                    },
                  });
                }

                const y = valueToMeasure(item.value);
                const barHeight = chartHeight - y;
                const x = i * slotSize + barPadding;
                const w = slotSize - barPadding * 2;
                return m('rect.pf-bar-chart__bar', {
                  x,
                  y,
                  width: Math.max(w, 1),
                  height: barHeight,
                  className: classNames(
                    isHovered && 'pf-bar-chart__bar--hover',
                  ),
                  onmouseenter: () => {
                    this.hoveredItem = item;
                  },
                  onmouseleave: () => {
                    this.hoveredItem = undefined;
                  },
                });
              }),

              // Brush selection rectangle
              this.brush.renderSelection(
                chartWidth,
                chartHeight,
                brushDir,
                'pf-bar-chart__brush-selection',
              ),

              // Axes
              ...(horizontal
                ? renderHorizontalAxes({
                    chartWidth,
                    chartHeight,
                    slotSize,
                    data,
                    measureTicks,
                    valueToMeasure,
                    formatMeasure,
                    formatLabel,
                    dimensionLabel,
                    measureLabel,
                  })
                : renderVerticalAxes({
                    chartWidth,
                    chartHeight,
                    slotSize,
                    data,
                    measureTicks,
                    valueToMeasure,
                    formatMeasure,
                    formatLabel,
                    dimensionLabel,
                    measureLabel,
                  })),
            ],
          ),
        ],
      ),
      // Tooltip
      this.hoveredItem &&
        m(
          '.pf-bar-chart__tooltip',
          m('.pf-bar-chart__tooltip-content', [
            m(
              '.pf-bar-chart__tooltip-row',
              formatLabel(this.hoveredItem.label),
            ),
            m(
              '.pf-bar-chart__tooltip-row',
              `${measureLabel}: ${formatMeasure(this.hoveredItem.value)}`,
            ),
          ]),
        ),
    );
  }
}

interface AxesParams {
  chartWidth: number;
  chartHeight: number;
  slotSize: number;
  data: BarChartData;
  measureTicks: number[];
  valueToMeasure: (v: number) => number;
  formatMeasure: (v: number) => string;
  formatLabel: (l: string | number) => string;
  dimensionLabel: string | undefined;
  measureLabel: string | undefined;
}

/**
 * Render axes for vertical orientation (dimension on X, measure on Y).
 */
function renderVerticalAxes(p: AxesParams): m.Children[] {
  return [
    // X Axis (dimension: labels)
    m('g.pf-bar-chart__x-axis', {transform: `translate(0, ${p.chartHeight})`}, [
      m('line.pf-bar-chart__axis-line', {
        x1: 0,
        y1: 0,
        x2: p.chartWidth,
        y2: 0,
      }),
      ...p.data.items.map((item, i) => {
        const x = i * p.slotSize + p.slotSize / 2;
        return m(
          'text.pf-bar-chart__tick-label',
          {
            'x': x,
            'y': 15,
            'text-anchor': 'middle',
            'dominant-baseline': 'middle',
          },
          truncateLabelToWidth(p.formatLabel(item.label), p.slotSize),
        );
      }),
      p.dimensionLabel &&
        m(
          'text.pf-bar-chart__axis-label',
          {
            'x': p.chartWidth / 2,
            'y': 35,
            'text-anchor': 'middle',
          },
          p.dimensionLabel,
        ),
    ]),

    // Y Axis (measure: values)
    m('g.pf-bar-chart__y-axis', [
      m('line.pf-bar-chart__axis-line', {
        x1: 0,
        y1: 0,
        x2: 0,
        y2: p.chartHeight,
      }),
      ...p.measureTicks.map((tick) => {
        const y = p.valueToMeasure(tick);
        return m('g', {transform: `translate(0, ${y})`}, [
          m('line.pf-bar-chart__tick', {x2: -5}),
          m(
            'text.pf-bar-chart__tick-label',
            {
              'x': -8,
              'text-anchor': 'end',
              'dominant-baseline': 'middle',
            },
            p.formatMeasure(tick),
          ),
        ]);
      }),
      p.measureLabel &&
        m(
          'text.pf-bar-chart__axis-label',
          {
            'transform': `translate(-50, ${p.chartHeight / 2}) rotate(-90)`,
            'text-anchor': 'middle',
          },
          p.measureLabel,
        ),
    ]),
  ];
}

/**
 * Render axes for horizontal orientation (measure on X, dimension on Y).
 */
function renderHorizontalAxes(p: AxesParams): m.Children[] {
  return [
    // X Axis (measure: values)
    m('g.pf-bar-chart__x-axis', {transform: `translate(0, ${p.chartHeight})`}, [
      m('line.pf-bar-chart__axis-line', {
        x1: 0,
        y1: 0,
        x2: p.chartWidth,
        y2: 0,
      }),
      ...p.measureTicks.map((tick) => {
        const x = p.valueToMeasure(tick);
        return m('g', {transform: `translate(${x}, 0)`}, [
          m('line.pf-bar-chart__tick', {y2: 5}),
          m(
            'text.pf-bar-chart__tick-label',
            {
              'y': 15,
              'text-anchor': 'middle',
              'dominant-baseline': 'middle',
            },
            p.formatMeasure(tick),
          ),
        ]);
      }),
      p.measureLabel &&
        m(
          'text.pf-bar-chart__axis-label',
          {
            'x': p.chartWidth / 2,
            'y': 30,
            'text-anchor': 'middle',
          },
          p.measureLabel,
        ),
    ]),

    // Y Axis (dimension: labels)
    m('g.pf-bar-chart__y-axis', [
      m('line.pf-bar-chart__axis-line', {
        x1: 0,
        y1: 0,
        x2: 0,
        y2: p.chartHeight,
      }),
      ...p.data.items.map((item, i) => {
        const y = i * p.slotSize + p.slotSize / 2;
        return m(
          'text.pf-bar-chart__tick-label',
          {
            'x': -8,
            'y': y,
            'text-anchor': 'end',
            'dominant-baseline': 'middle',
          },
          truncateLabelToWidth(
            p.formatLabel(item.label),
            MARGIN_HORIZONTAL.left - 12,
          ),
        );
      }),
      p.dimensionLabel &&
        m(
          'text.pf-bar-chart__axis-label',
          {
            'transform': `translate(-65, ${p.chartHeight / 2}) rotate(-90)`,
            'text-anchor': 'middle',
          },
          p.dimensionLabel,
        ),
    ]),
  ];
}

/**
 * Truncate a label to fit within a given pixel width (approximate).
 * Uses ~6px per character estimate at font-size 10px in SVG.
 */
function truncateLabelToWidth(label: string, maxWidth: number): string {
  const maxChars = Math.max(3, Math.floor(maxWidth / 6));
  return truncateLabel(label, maxChars);
}

/**
 * Aggregate raw data into BarChartData by grouping on a dimension and
 * applying an aggregation function to the measure values.
 *
 * Results are sorted by aggregated value (descending).
 *
 * @param items The raw data items to aggregate.
 * @param dimension Extracts the grouping key (bar label) from each item.
 * @param measure Extracts the numeric value from each item.
 * @param aggregation The aggregation function to apply per group.
 */
export function aggregateBarChartData<T>(
  items: readonly T[],
  dimension: (item: T) => string | number,
  measure: (item: T) => number,
  aggregation: AggregationType,
): BarChartData {
  const groups = new Map<string | number, number[]>();
  for (const item of items) {
    const key = dimension(item);
    let values = groups.get(key);
    if (values === undefined) {
      values = [];
      groups.set(key, values);
    }
    values.push(measure(item));
  }

  const result: BarChartItem[] = [];
  for (const [label, values] of groups) {
    result.push({label, value: aggregate(values, aggregation)});
  }

  result.sort((a, b) => b.value - a.value);
  return {items: result};
}

function aggregate(values: number[], agg: AggregationType): number {
  switch (agg) {
    case 'SUM':
      return values.reduce((a, b) => a + b, 0);
    case 'AVG':
      return values.reduce((a, b) => a + b, 0) / values.length;
    case 'MIN':
      return values.reduce((a, b) => Math.min(a, b), Infinity);
    case 'MAX':
      return values.reduce((a, b) => Math.max(a, b), -Infinity);
    case 'COUNT':
      return values.length;
    case 'COUNT_DISTINCT':
      return new Set(values).size;
  }
}
