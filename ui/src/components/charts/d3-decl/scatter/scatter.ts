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

/**
 * Scatter plot chart component.
 *
 * Displays relationships between two numeric variables as points, supporting:
 * - 2D rectangular brush selection
 * - Click-on-point to filter single point
 * - Click-on-empty to clear filters
 * - Hover tooltips with point details
 * - Optional correlation line
 * - Category-based coloring
 *
 * State management: Always controlled by parent.
 * Parent owns filters array, chart emits new array on interaction.
 * No m.redraw() needed - Mithril auto-redraws after event handlers.
 *
 * @example
 * ```typescript
 * class Dashboard {
 *   private filters: Filter[] = [];
 *
 *   view() {
 *     return m(ScatterPlot, {
 *       data: scatterLoader.use({filters: this.filters}),
 *       filters: this.filters,
 *       xColumn: 'duration',
 *       yColumn: 'cpu_time',
 *       onFiltersChanged: (filters) => { this.filters = filters; },
 *     });
 *   }
 * }
 * ```
 */

import m from 'mithril';
import * as d3 from 'd3';
import {classNames} from '../../../../base/classnames';
import {Spinner} from '../../../../widgets/spinner';
import {Filter} from '../../../../components/widgets/datagrid/model';
import {
  DEFAULT_MARGIN,
  VIEWBOX_WIDTH,
  LEGEND_WIDTH,
  formatNumber,
} from '../chart_utils';
import {renderLinearAxis, renderGridLines} from '../renderers/axis_renderer';
import {BrushHandler2D} from '../interactions/brush_handler';
import {renderLegend} from '../renderers/legend_renderer';

/**
 * A single point in the scatter plot.
 */
export interface ScatterPoint {
  /** X-axis value */
  readonly x: number;
  /** Y-axis value */
  readonly y: number;
  /** Optional label for tooltip */
  readonly label?: string;
  /** Optional category for coloring */
  readonly category?: string;
}

/**
 * Correlation statistics for the scatter plot.
 */
export interface CorrelationStats {
  /** Pearson correlation coefficient [-1, 1] */
  readonly r: number;
  /** Slope of regression line */
  readonly slope: number;
  /** Y-intercept of regression line */
  readonly intercept: number;
}

/**
 * Data for the scatter plot.
 */
export interface ScatterData {
  /** Points to display */
  readonly points: readonly ScatterPoint[];
  /** Minimum X value */
  readonly xMin: number;
  /** Maximum X value */
  readonly xMax: number;
  /** Minimum Y value */
  readonly yMin: number;
  /** Maximum Y value */
  readonly yMax: number;
  /** Optional correlation statistics */
  readonly correlation?: CorrelationStats;
  /** Optional list of categories for legend */
  readonly categories?: readonly string[];
}

/**
 * Scatter plot component attributes following clean patterns.
 */
export interface ScatterPlotAttrs {
  /**
   * Scatter data to display, or undefined if loading.
   */
  readonly data: ScatterData | undefined;

  /**
   * Complete filter array. Chart uses this to:
   * 1. Show filter overlays (e.g., brush rectangles)
   * 2. Compute new filters when user interacts
   */
  readonly filters: readonly Filter[];

  /**
   * X-axis column name. Used to identify which filters belong to X dimension.
   */
  readonly xColumn: string;

  /**
   * Y-axis column name. Used to identify which filters belong to Y dimension.
   */
  readonly yColumn: string;

  /**
   * Called when user adds or removes filters via interaction.
   * Chart returns COMPLETE new filter array, not just changes.
   * Parent should assign: this.filters = newFilters
   * No m.redraw() needed - Mithril auto-redraws after event handlers.
   */
  readonly onFiltersChanged?: (filters: readonly Filter[]) => void;

  /**
   * Display height in pixels. Defaults to 200.
   */
  readonly height?: number;

  /**
   * X axis label.
   */
  readonly xAxisLabel?: string;

  /**
   * Y axis label.
   */
  readonly yAxisLabel?: string;

  /**
   * Custom value formatter for X axis.
   */
  readonly formatXValue?: (value: number) => string;

  /**
   * Custom value formatter for Y axis.
   */
  readonly formatYValue?: (value: number) => string;

  /**
   * Point color (for single-category charts).
   */
  readonly pointColor?: string;

  /**
   * Point radius in pixels. Defaults to 4.
   */
  readonly pointSize?: number;

  /**
   * Show correlation line and stats.
   */
  readonly showCorrelation?: boolean;

  /**
   * Fill parent container.
   */
  readonly fillParent?: boolean;

  /**
   * Additional CSS class.
   */
  readonly className?: string;
}

const DEFAULT_HEIGHT = 200;
const DEFAULT_POINT_SIZE = 4;

/**
 * Scatter plot component.
 *
 * Pure view function - state flows down, events flow up.
 * Only stores UI transient state (hover, brush in progress).
 */
export class ScatterPlot implements m.ClassComponent<ScatterPlotAttrs> {
  // UI transient state only - no data state
  private hoveredPoint?: ScatterPoint;
  private brushHandler?: BrushHandler2D;
  private svgElement?: SVGSVGElement;
  // Store current attrs to avoid stale closures in brush handler
  private currentAttrs?: ScatterPlotAttrs;

  view({attrs}: m.Vnode<ScatterPlotAttrs>): m.Children {
    // Update cached attrs for callbacks (similar to React useRef pattern)
    this.currentAttrs = attrs;
    const {
      data,
      onFiltersChanged,
      height = DEFAULT_HEIGHT,
      xAxisLabel,
      yAxisLabel,
      formatXValue = formatNumber,
      formatYValue = formatNumber,
      pointColor,
      pointSize = DEFAULT_POINT_SIZE,
      showCorrelation = false,
      fillParent = false,
      className,
    } = attrs;

    // Loading state
    if (data === undefined) {
      return this.renderLoading(height, fillParent, className);
    }

    // Empty state
    if (data.points.length === 0) {
      return this.renderEmpty(height, fillParent, className);
    }

    // Calculate dimensions
    const chartWidth =
      VIEWBOX_WIDTH - DEFAULT_MARGIN.left - DEFAULT_MARGIN.right;
    const chartHeight = height - DEFAULT_MARGIN.top - DEFAULT_MARGIN.bottom;

    // Create scales
    const xScale = d3
      .scaleLinear()
      .domain([data.xMin, data.xMax])
      .nice()
      .range([0, chartWidth]);

    const yScale = d3
      .scaleLinear()
      .domain([data.yMin, data.yMax])
      .nice()
      .range([chartHeight, 0]);

    const colorScale = this.createColorScale(data.categories, pointColor);

    // Initialize or update brush handler
    if (onFiltersChanged && this.svgElement) {
      if (!this.brushHandler) {
        this.brushHandler = new BrushHandler2D(
          this.svgElement,
          xScale,
          yScale,
          DEFAULT_MARGIN,
          chartWidth,
          chartHeight,
          (rect) => {
            // Access current attrs via instance (avoids stale closure)
            if (this.currentAttrs) {
              this.handleBrush(rect, data, this.currentAttrs);
            }
          },
          () => {
            // Access current attrs via instance (avoids stale closure)
            if (this.currentAttrs) {
              this.handleClearFilters(this.currentAttrs);
            }
          },
        );
      } else {
        // Update scales on re-render (e.g., after filtering changes domains)
        this.brushHandler.updateScales(xScale, yScale);
      }
    }

    return m(
      '.pf-d3-scatter',
      {
        class: classNames(
          fillParent && 'pf-d3-scatter--fill-parent',
          className,
        ),
        style: {height: `${height}px`},
      },
      [
        m(
          'svg.pf-d3-scatter__svg',
          {
            viewBox: `0 0 ${data.categories && data.categories.length > 1 ? VIEWBOX_WIDTH + LEGEND_WIDTH : VIEWBOX_WIDTH} ${height}`,
            preserveAspectRatio: 'xMidYMid meet',
            oncreate: (vnode) => {
              this.svgElement = vnode.dom as SVGSVGElement;
            },
          },
          [
            // Chart area group
            m(
              'g.pf-d3-scatter__chart-area',
              {
                transform: `translate(${DEFAULT_MARGIN.left}, ${DEFAULT_MARGIN.top})`,
                style: onFiltersChanged ? {cursor: 'crosshair'} : undefined,
                ...(this.brushHandler?.getEventHandlers() ?? {}),
              },
              [
                // Background for click capture
                m('rect.pf-d3-scatter__background', {
                  x: 0,
                  y: 0,
                  width: chartWidth,
                  height: chartHeight,
                  fill: 'transparent',
                }),

                // Horizontal grid lines
                renderGridLines({
                  scale: yScale,
                  orientation: 'horizontal',
                  length: chartWidth,
                }),

                // Vertical grid lines
                renderGridLines({
                  scale: xScale,
                  orientation: 'vertical',
                  length: chartHeight,
                }),

                // Correlation line (behind points)
                showCorrelation &&
                  data.correlation &&
                  this.renderCorrelationLine(
                    data.correlation,
                    xScale,
                    yScale,
                    chartWidth,
                  ),

                // Data points
                ...data.points.map((point) => {
                  const cx = xScale(point.x);
                  const cy = yScale(point.y);
                  const color = point.category
                    ? colorScale(point.category)
                    : pointColor || 'var(--pf-d3-scatter-point-color)';
                  const isHovered = this.hoveredPoint === point;

                  return m('circle.pf-d3-scatter__point', {
                    cx,
                    cy,
                    r: isHovered ? pointSize * 1.5 : pointSize,
                    fill: color,
                    class: classNames(
                      isHovered && 'pf-d3-scatter__point--hover',
                    ),
                    onmouseenter: () => {
                      this.hoveredPoint = point;
                    },
                    onmouseleave: () => {
                      this.hoveredPoint = undefined;
                    },
                  });
                }),

                // Brush overlay (in-progress selection)
                this.renderBrushOverlay(xScale, yScale),

                // X Axis
                m(
                  'g',
                  {transform: `translate(0, ${chartHeight})`},
                  renderLinearAxis({
                    scale: xScale,
                    orientation: 'bottom',
                    length: chartWidth,
                    label: xAxisLabel,
                    tickFormatter: formatXValue,
                  }),
                ),

                // Y Axis
                renderLinearAxis({
                  scale: yScale,
                  orientation: 'left',
                  length: chartHeight,
                  label: yAxisLabel,
                  tickFormatter: formatYValue,
                }),
              ],
            ),

            // Correlation stats - in right margin, above legend
            showCorrelation &&
              data.correlation &&
              m(
                'g',
                {
                  transform: `translate(${VIEWBOX_WIDTH + 100}, ${DEFAULT_MARGIN.top})`,
                },
                [
                  m(
                    'text.pf-d3-scatter__correlation-label',
                    {
                      'x': -18,
                      'y': 6,
                      'text-anchor': 'end',
                      'dominant-baseline': 'middle',
                      'style': 'font-size: 11px; fill: var(--pf-color-text);',
                    },
                    'Correlation:',
                  ),
                  m(
                    'text.pf-d3-scatter__correlation-value',
                    {
                      'x': -18,
                      'y': 22,
                      'text-anchor': 'end',
                      'dominant-baseline': 'middle',
                      'style':
                        'font-size: 11px; font-weight: 600; fill: var(--pf-color-text);',
                    },
                    `r = ${data.correlation.r.toFixed(3)}`,
                  ),
                ],
              ),

            // Legend (for multi-category charts) - in right margin, below correlation
            data.categories &&
              data.categories.length > 1 &&
              m(
                'g',
                {
                  transform: `translate(${VIEWBOX_WIDTH + 100}, ${
                    DEFAULT_MARGIN.top +
                    (showCorrelation && data.correlation ? 40 : 0)
                  })`,
                },
                renderLegend({
                  items: data.categories.map((category) => ({
                    name: category,
                    color: colorScale(category),
                  })),
                  position: 'top-right',
                  chartWidth: 0,
                }),
              ),
          ],
        ),

        // Tooltip
        this.hoveredPoint &&
          this.renderTooltip(this.hoveredPoint, formatXValue, formatYValue),
      ],
    );
  }

  private renderBrushOverlay(
    xScale: d3.ScaleLinear<number, number>,
    yScale: d3.ScaleLinear<number, number>,
  ): m.Children {
    const brush = this.brushHandler?.getCurrentBrush();
    if (!brush) return null;

    const x = Math.min(xScale(brush.xMin), xScale(brush.xMax));
    const y = Math.min(yScale(brush.yMin), yScale(brush.yMax));
    const w = Math.abs(xScale(brush.xMax) - xScale(brush.xMin));
    const h = Math.abs(yScale(brush.yMax) - yScale(brush.yMin));

    return m('rect.pf-d3-scatter__brush-selection', {
      x,
      y,
      width: w,
      height: h,
    });
  }

  private handleBrush(
    rect: {xMin: number; xMax: number; yMin: number; yMax: number},
    data: ScatterData,
    attrs: ScatterPlotAttrs,
  ): void {
    if (!attrs.onFiltersChanged) return;

    // Check if any points are within the brush bounds
    const hasPointsInBounds = data.points.some(
      (point) =>
        point.x >= rect.xMin &&
        point.x <= rect.xMax &&
        point.y >= rect.yMin &&
        point.y <= rect.yMax,
    );

    if (hasPointsInBounds) {
      // Filter to brushed region
      const newFilters: Filter[] = [
        ...attrs.filters.filter(
          (f) => f.field !== attrs.xColumn && f.field !== attrs.yColumn,
        ),
        {field: attrs.xColumn, op: '>=', value: rect.xMin},
        {field: attrs.xColumn, op: '<=', value: rect.xMax},
        {field: attrs.yColumn, op: '>=', value: rect.yMin},
        {field: attrs.yColumn, op: '<=', value: rect.yMax},
      ];
      attrs.onFiltersChanged(newFilters);
    }
    // If no points in bounds, do nothing (don't create filter)
  }

  private handleClearFilters(attrs: ScatterPlotAttrs): void {
    if (!attrs.onFiltersChanged) return;

    // Remove all filters for both columns
    const newFilters = attrs.filters.filter(
      (f) => f.field !== attrs.xColumn && f.field !== attrs.yColumn,
    );
    attrs.onFiltersChanged(newFilters);
  }

  private createColorScale(
    categories: readonly string[] | undefined,
    singleColor: string | undefined,
  ): (category: string) => string {
    if (!categories || categories.length === 0) {
      return () => singleColor || 'var(--pf-d3-scatter-point-color)';
    }
    const scale = d3
      .scaleOrdinal<string>()
      .domain([...categories])
      .range(d3.schemeCategory10);
    return (category: string) => scale(category);
  }

  private renderCorrelationLine(
    correlation: CorrelationStats,
    xScale: d3.ScaleLinear<number, number>,
    yScale: d3.ScaleLinear<number, number>,
    _chartWidth: number,
  ): m.Children {
    const xDomain = xScale.domain();
    const yDomain = yScale.domain();
    const x1 = xDomain[0];
    const x2 = xDomain[1];
    const y1 = correlation.slope * x1 + correlation.intercept;
    const y2 = correlation.slope * x2 + correlation.intercept;

    // Clamp y values to stay within chart bounds (don't cross axes)
    const y1Clamped = Math.max(yDomain[0], Math.min(yDomain[1], y1));
    const y2Clamped = Math.max(yDomain[0], Math.min(yDomain[1], y2));

    return m('line.pf-d3-scatter__correlation-line', {
      x1: xScale(x1),
      y1: yScale(y1Clamped),
      x2: xScale(x2),
      y2: yScale(y2Clamped),
    });
  }

  private renderLoading(
    height: number,
    fillParent: boolean,
    className?: string,
  ): m.Children {
    return m(
      '.pf-d3-scatter',
      {
        class: classNames(
          fillParent && 'pf-d3-scatter--fill-parent',
          className,
        ),
        style: {height: `${height}px`},
      },
      m('.pf-d3-scatter__loading', m(Spinner)),
    );
  }

  private renderEmpty(
    height: number,
    fillParent: boolean,
    className?: string,
  ): m.Children {
    return m(
      '.pf-d3-scatter',
      {
        class: classNames(
          fillParent && 'pf-d3-scatter--fill-parent',
          className,
        ),
        style: {height: `${height}px`},
      },
      m('.pf-d3-scatter__empty', 'No data to display'),
    );
  }

  private renderTooltip(
    point: ScatterPoint,
    formatXValue: (value: number) => string,
    formatYValue: (value: number) => string,
  ): m.Children {
    return m(
      '.pf-d3-scatter__tooltip',
      m('.pf-d3-scatter__tooltip-content', [
        ...(point.label ? [m('.pf-d3-scatter__tooltip-row', point.label)] : []),
        ...(point.category
          ? [m('.pf-d3-scatter__tooltip-row', point.category)]
          : []),
        m(
          '.pf-d3-scatter__tooltip-row',
          `${formatXValue(point.x)}, ${formatYValue(point.y)}`,
        ),
      ]),
    );
  }
}
