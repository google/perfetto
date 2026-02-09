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
 * CDF (Cumulative Distribution Function) chart component.
 *
 * Displays cumulative probability distributions as line charts, supporting:
 * - Multiple CDF lines (overlays)
 * - Brush selection for filtering
 * - Click-on-empty to clear filters
 * - Crosshair with probability tooltips
 * - Optional percentile markers (P50, P90, P95, P99)
 * - Correlation/comparison between distributions
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
 *     return m(CDFChart, {
 *       data: cdfLoader.use({filters: this.filters}),
 *       filters: this.filters,
 *       column: 'duration',
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
import {renderLegend} from '../renderers/legend_renderer';
import {BrushHandler1D} from '../interactions/brush_handler';

/**
 * A single point on a CDF curve.
 */
export interface CDFPoint {
  /** Value on X-axis (e.g., duration) */
  readonly value: number;
  /** Cumulative probability [0, 1] */
  readonly probability: number;
}

/**
 * A single CDF line (for overlaying multiple distributions).
 */
export interface CDFLine {
  /** Line label for legend */
  readonly name: string;
  /** Points defining the curve */
  readonly points: readonly CDFPoint[];
  /** Optional custom color */
  readonly color?: string;
}

/**
 * Data for the CDF chart.
 */
export interface CDFData {
  /** CDF lines to display */
  readonly lines: readonly CDFLine[];
  /** Minimum value across all lines */
  readonly min: number;
  /** Maximum value across all lines */
  readonly max: number;
}

/**
 * CDF chart component attributes following clean patterns.
 */
export interface CDFChartAttrs {
  /**
   * CDF data to display, or undefined if loading.
   */
  readonly data: CDFData | undefined;

  /**
   * Complete filter array. Chart uses this to:
   * 1. Show filter overlays (e.g., brush rectangles)
   * 2. Compute new filters when user interacts
   */
  readonly filters: readonly Filter[];

  /**
   * Column/field name this CDF operates on (X-axis).
   * Used to identify which filters belong to this chart.
   */
  readonly column: string;

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
   * Y axis label. Defaults to 'Cumulative Probability'.
   */
  readonly yAxisLabel?: string;

  /**
   * Custom value formatter for X axis.
   */
  readonly formatXValue?: (value: number) => string;

  /**
   * Show percentile markers (P50, P90, P95, P99).
   */
  readonly showPercentiles?: boolean;

  /**
   * Called when user clicks on a line (for filtering by distribution).
   */
  readonly onLineClick?: (lineName: string) => void;

  /**
   * Fill parent container.
   */
  readonly fillParent?: boolean;

  /**
   * Additional CSS class.
   */
  readonly className?: string;

  /**
   * Optional title displayed above chart.
   */
  readonly title?: string;
}

const DEFAULT_HEIGHT = 200;
const PERCENTILES = [0.5, 0.9, 0.95, 0.99];

/**
 * CDF chart component.
 *
 * Pure view function - state flows down, events flow up.
 * Only stores UI transient state (crosshair, brush in progress).
 */
export class CDFChart implements m.ClassComponent<CDFChartAttrs> {
  // UI transient state only - no data state
  private crosshairX?: number;
  private brushHandler?: BrushHandler1D;
  private svgElement?: SVGSVGElement;
  // Store current attrs to avoid stale closures in brush handler
  private currentAttrs?: CDFChartAttrs;

  view({attrs}: m.Vnode<CDFChartAttrs>): m.Children {
    // Update cached attrs for callbacks (similar to React useRef pattern)
    this.currentAttrs = attrs;
    const {
      data,
      filters,
      column,
      onFiltersChanged,
      height = DEFAULT_HEIGHT,
      xAxisLabel,
      yAxisLabel = 'Cumulative Probability',
      formatXValue = formatNumber,
      showPercentiles = false,
      onLineClick,
      fillParent = false,
      className,
      title,
    } = attrs;

    const hasActiveFilter = filters.some((f) => f.field === column);

    // Loading state
    if (data === undefined) {
      return this.renderLoading(height, fillParent, className);
    }

    // Empty state
    if (
      data.lines.length === 0 ||
      data.lines.every((l) => l.points.length === 0)
    ) {
      return this.renderEmpty(height, fillParent, className);
    }

    // Calculate dimensions
    const chartWidth =
      VIEWBOX_WIDTH - DEFAULT_MARGIN.left - DEFAULT_MARGIN.right;
    const chartHeight = height - DEFAULT_MARGIN.top - DEFAULT_MARGIN.bottom;

    // Create scales
    const xScale = d3
      .scaleLinear()
      .domain([data.min, data.max])
      .range([0, chartWidth]);

    const yScale = d3.scaleLinear().domain([0, 1]).range([chartHeight, 0]);

    // Initialize or update brush handler
    if (onFiltersChanged && this.svgElement) {
      if (!this.brushHandler) {
        this.brushHandler = new BrushHandler1D(
          this.svgElement,
          xScale,
          DEFAULT_MARGIN,
          chartWidth,
          (start, end) => {
            // Access current attrs via instance (avoids stale closure)
            if (this.currentAttrs) {
              this.handleBrush(start, end, this.currentAttrs);
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
        // Update scale on re-render (important after filtering changes domain)
        this.brushHandler.updateScale(xScale);
      }
    }

    // Create D3 line generator
    const line = d3
      .line<CDFPoint>()
      .x((d) => xScale(d.value))
      .y((d) => yScale(d.probability));

    const colorScale = this.createColorScale(data.lines);

    return m(
      '.pf-d3-cdf',
      {
        class: classNames(fillParent && 'pf-d3-cdf--fill-parent', className),
        style: {height: `${height}px`},
      },
      [
        // Optional header with title and filter indicator
        title &&
          m(
            '.pf-d3-cdf__header',
            {
              class: classNames(
                hasActiveFilter && 'pf-d3-cdf__header--active-filter',
              ),
            },
            m('.pf-d3-cdf__title', title),
          ),
        m(
          'svg.pf-d3-cdf__svg',
          {
            viewBox: `0 0 ${data.lines.length > 1 ? VIEWBOX_WIDTH + LEGEND_WIDTH : VIEWBOX_WIDTH} ${height}`,
            preserveAspectRatio: 'xMidYMid meet',
            oncreate: (vnode) => {
              this.svgElement = vnode.dom as SVGSVGElement;
            },
          },
          [
            // Chart area group
            m(
              'g.pf-d3-cdf__chart-area',
              {
                transform: `translate(${DEFAULT_MARGIN.left}, ${DEFAULT_MARGIN.top})`,
                style: onFiltersChanged ? {cursor: 'crosshair'} : undefined,
                ...(this.brushHandler?.getEventHandlers() ?? {}),
              },
              [
                // Background for click capture and crosshair tracking
                m('rect.pf-d3-cdf__background', {
                  x: 0,
                  y: 0,
                  width: chartWidth,
                  height: chartHeight,
                  fill: 'transparent',
                  onpointermove: (e: PointerEvent) => {
                    // Update crosshair position
                    this.crosshairX = this.clientXToValue(
                      e,
                      xScale,
                      chartWidth,
                    );
                  },
                  onpointerleave: () => {
                    this.crosshairX = undefined;
                  },
                }),

                // Horizontal grid lines
                renderGridLines({
                  scale: yScale,
                  orientation: 'horizontal',
                  length: chartWidth,
                }),

                // CDF Lines
                ...data.lines.map((cdfLine) => {
                  const pathData = line(cdfLine.points as CDFPoint[]);
                  const color = cdfLine.color || colorScale(cdfLine.name);

                  return m('path.pf-d3-cdf__line', {
                    'd': pathData || '',
                    'stroke': color,
                    'fill': 'none',
                    'stroke-width': 2,
                    'style': {cursor: onLineClick ? 'pointer' : 'default'},
                    'onclick': onLineClick
                      ? () => onLineClick(cdfLine.name)
                      : undefined,
                  });
                }),

                // Percentile markers
                ...(showPercentiles && data.lines.length > 0
                  ? PERCENTILES.map((p) =>
                      this.renderPercentileLine(
                        p,
                        data.lines[0],
                        xScale,
                        yScale,
                        chartHeight,
                        formatXValue,
                      ),
                    )
                  : []),

                // Brush overlay (in-progress selection)
                this.renderBrushOverlay(xScale, chartHeight),

                // Crosshair line and dots
                this.crosshairX !== undefined &&
                  this.renderCrosshairLine(
                    this.crosshairX,
                    data.lines,
                    xScale,
                    yScale,
                    chartHeight,
                    colorScale,
                  ),

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

                // Y Axis (Probability)
                renderLinearAxis({
                  scale: yScale,
                  orientation: 'left',
                  length: chartHeight,
                  label: yAxisLabel,
                  tickFormatter: (v) => `${(v * 100).toFixed(0)}%`,
                }),
              ],
            ),

            // Legend (for multi-line charts) - in right margin
            data.lines.length > 1 &&
              m(
                'g',
                {
                  transform: `translate(${VIEWBOX_WIDTH + 100}, ${DEFAULT_MARGIN.top})`,
                },
                renderLegend({
                  items: data.lines.map((cdfLine) => ({
                    name: cdfLine.name,
                    color: cdfLine.color || colorScale(cdfLine.name),
                  })),
                  position: 'top-right',
                  chartWidth: 0, // Legend positioned absolutely, no need for chartWidth offset
                }),
              ),
          ],
        ),

        // Tooltip (outside SVG for fixed positioning)
        this.crosshairX !== undefined &&
          this.renderTooltip(
            this.crosshairX,
            data.lines,
            formatXValue,
            colorScale,
          ),
      ],
    );
  }

  private renderBrushOverlay(
    xScale: d3.ScaleLinear<number, number>,
    chartHeight: number,
  ): m.Children {
    const brush = this.brushHandler?.getCurrentBrush();
    if (!brush) return null;

    const startX = xScale(brush.start);
    const endX = xScale(brush.end);

    // Ensure we draw from left to right regardless of drag direction
    const x = Math.min(startX, endX);
    const width = Math.abs(endX - startX);

    return m('rect.pf-d3-cdf__brush-selection', {
      x,
      y: 0,
      width,
      height: chartHeight,
    });
  }

  private handleBrush(start: number, end: number, attrs: CDFChartAttrs): void {
    if (!attrs.onFiltersChanged) return;

    // Brush selection - filter to range using >= and <= operators
    const newFilters: Filter[] = [
      ...attrs.filters.filter((f) => f.field !== attrs.column),
      {field: attrs.column, op: '>=', value: start},
      {field: attrs.column, op: '<=', value: end},
    ];

    attrs.onFiltersChanged(newFilters);
  }

  private handleClearFilters(attrs: CDFChartAttrs): void {
    if (!attrs.onFiltersChanged) return;

    // Remove all filters for this column
    const newFilters = attrs.filters.filter((f) => f.field !== attrs.column);
    attrs.onFiltersChanged(newFilters);
  }

  private clientXToValue(
    e: PointerEvent,
    xScale: d3.ScaleLinear<number, number>,
    chartWidth: number,
  ): number {
    const svg = this.svgElement;
    if (!svg) return 0;

    // Use SVG's built-in coordinate transformation
    const pt = svg.createSVGPoint();
    pt.x = e.clientX;
    pt.y = e.clientY;

    const svgP = pt.matrixTransform(svg.getScreenCTM()!.inverse());
    const chartX = svgP.x - DEFAULT_MARGIN.left;

    // Clamp to chart bounds
    const clampedX = Math.max(0, Math.min(chartWidth, chartX));

    return xScale.invert(clampedX);
  }

  private createColorScale(
    lines: readonly CDFLine[],
  ): (name: string) => string {
    const names = lines.map((l) => l.name);
    const scale = d3
      .scaleOrdinal<string>()
      .domain(names)
      .range(d3.schemeCategory10);
    return (name: string) => scale(name);
  }

  private renderPercentileLine(
    percentile: number,
    line: CDFLine,
    xScale: d3.ScaleLinear<number, number>,
    _yScale: d3.ScaleLinear<number, number>,
    chartHeight: number,
    formatXValue: (value: number) => string,
  ): m.Children {
    const point = line.points.find((p) => p.probability >= percentile);
    if (!point) return null;

    const x = xScale(point.value);

    return m('g.pf-d3-cdf__percentile', [
      m('line.pf-d3-cdf__percentile-line', {
        x1: x,
        y1: 0,
        x2: x,
        y2: chartHeight,
      }),
      m(
        'text.pf-d3-cdf__percentile-label',
        {
          'x': x,
          'y': -5,
          'text-anchor': 'middle',
          'font-size': '10px',
        },
        `P${percentile * 100}`,
      ),
      m(
        'text.pf-d3-cdf__percentile-value',
        {
          'x': x,
          'y': chartHeight + 15,
          'text-anchor': 'middle',
          'font-size': '9px',
        },
        formatXValue(point.value),
      ),
    ]);
  }

  private renderCrosshairLine(
    xValue: number,
    lines: readonly CDFLine[],
    xScale: d3.ScaleLinear<number, number>,
    yScale: d3.ScaleLinear<number, number>,
    chartHeight: number,
    colorScale: (name: string) => string,
  ): m.Children {
    const x = xScale(xValue);

    const lineValues: Array<{
      probability: number;
      color: string;
    }> = [];

    for (const line of lines) {
      if (line.points.length === 0) continue;

      // Find closest point to crosshair
      let closestPoint = line.points[0];
      let minDist = Math.abs(closestPoint.value - xValue);

      for (const point of line.points) {
        const dist = Math.abs(point.value - xValue);
        if (dist < minDist) {
          minDist = dist;
          closestPoint = point;
        }
      }

      lineValues.push({
        probability: closestPoint.probability,
        color: line.color || colorScale(line.name),
      });
    }

    return m('g.pf-d3-cdf__crosshair', [
      m('line.pf-d3-cdf__crosshair-line', {
        x1: x,
        y1: 0,
        x2: x,
        y2: chartHeight,
      }),

      ...lineValues.map((lv) => {
        return m('circle.pf-d3-cdf__crosshair-dot', {
          cx: x,
          cy: yScale(lv.probability),
          r: 4,
          fill: lv.color,
        });
      }),
    ]);
  }

  private renderTooltip(
    xValue: number,
    lines: readonly CDFLine[],
    formatXValue: (value: number) => string,
    colorScale: (name: string) => string,
  ): m.Children {
    const lineValues: Array<{
      name: string;
      probability: number;
      color: string;
    }> = [];

    for (const line of lines) {
      if (line.points.length === 0) continue;

      // Find closest point to crosshair
      let closestPoint = line.points[0];
      let minDist = Math.abs(closestPoint.value - xValue);

      for (const point of line.points) {
        const dist = Math.abs(point.value - xValue);
        if (dist < minDist) {
          minDist = dist;
          closestPoint = point;
        }
      }

      lineValues.push({
        name: line.name,
        probability: closestPoint.probability,
        color: line.color || colorScale(line.name),
      });
    }

    return m(
      '.pf-d3-cdf__tooltip',
      m('.pf-d3-cdf__tooltip-content', [
        m('.pf-d3-cdf__tooltip-row', formatXValue(xValue)),
        ...lineValues.map((lv) =>
          m('.pf-d3-cdf__tooltip-row', [
            m(
              'span',
              {style: {color: lv.color, fontWeight: 'bold'}},
              lv.name === 'data' ? '' : `${lv.name}: `,
            ),
            m('span', `${(lv.probability * 100).toFixed(0)}%`),
          ]),
        ),
      ]),
    );
  }

  private renderLoading(
    height: number,
    fillParent: boolean,
    className?: string,
  ): m.Children {
    return m(
      '.pf-d3-cdf',
      {
        class: classNames(fillParent && 'pf-d3-cdf--fill-parent', className),
        style: {height: `${height}px`},
      },
      m('.pf-d3-cdf__loading', m(Spinner)),
    );
  }

  private renderEmpty(
    height: number,
    fillParent: boolean,
    className?: string,
  ): m.Children {
    return m(
      '.pf-d3-cdf',
      {
        class: classNames(fillParent && 'pf-d3-cdf--fill-parent', className),
        style: {height: `${height}px`},
      },
      m('.pf-d3-cdf__empty', 'No data to display'),
    );
  }
}
