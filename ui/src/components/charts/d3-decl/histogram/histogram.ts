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
 * Histogram chart component.
 *
 * Displays distribution of numeric values as bars, supporting:
 * - Linear and logarithmic Y scales
 * - Brush selection for filtering
 * - Click-on-bucket to filter single bucket
 * - Click-on-empty to clear filters
 * - Hover tooltips with percentage
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
 *     return m(Histogram, {
 *       data: histogramLoader.use({filters: this.filters}),
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
  HistogramBucket,
  HistogramData,
  HistogramConfig,
  computeHistogram,
} from './histogram_loader';
import {DEFAULT_MARGIN, VIEWBOX_WIDTH, formatNumber} from '../chart_utils';
import {renderLinearAxis, renderGridLines} from '../renderers/axis_renderer';
import {BrushHandler1D} from '../interactions/brush_handler';

// Re-export data types for convenience
export {HistogramBucket, HistogramData, HistogramConfig, computeHistogram};

/**
 * Histogram component attributes following clean patterns.
 */
export interface HistogramAttrs {
  /**
   * Histogram data to display, or undefined if loading.
   */
  readonly data: HistogramData | undefined;

  /**
   * Complete filter array. Chart uses this to:
   * 1. Show filter overlays (e.g., brush rectangles)
   * 2. Compute new filters when user interacts
   */
  readonly filters: readonly Filter[];

  /**
   * Column/field name this histogram operates on.
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
   * Y axis label. Defaults to 'Count'.
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
   * Bar color. Sets CSS variable.
   */
  readonly barColor?: string;

  /**
   * Bar hover color. Sets CSS variable.
   */
  readonly barHoverColor?: string;

  /**
   * Use logarithmic scale for Y axis. Defaults to false.
   */
  readonly logScale?: boolean;

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

/**
 * Histogram chart component.
 *
 * Pure view function - state flows down, events flow up.
 * Only stores UI transient state (hover, brush in progress).
 */
export class Histogram implements m.ClassComponent<HistogramAttrs> {
  // UI transient state only - no data state
  private hoveredBucket?: HistogramBucket;
  private brushHandler?: BrushHandler1D;
  private svgElement?: SVGSVGElement;
  // Store current attrs to avoid stale closures in brush handler
  private currentAttrs?: HistogramAttrs;

  // Stable event handler functions (created once, reused across renders)
  private readonly handlePointerDown = (e: PointerEvent) => {
    if (this.brushHandler) {
      const handlers = this.brushHandler.getEventHandlers();
      handlers.onpointerdown(e);
    }
  };

  private readonly handlePointerMove = (e: PointerEvent) => {
    if (this.brushHandler) {
      const handlers = this.brushHandler.getEventHandlers();
      handlers.onpointermove(e);
    }
  };

  private readonly handlePointerUp = (e: PointerEvent) => {
    if (this.brushHandler) {
      const handlers = this.brushHandler.getEventHandlers();
      handlers.onpointerup(e);
    }
  };

  private readonly handlePointerCancel = (e: PointerEvent) => {
    if (this.brushHandler) {
      const handlers = this.brushHandler.getEventHandlers();
      handlers.onpointercancel(e);
    }
  };

  view({attrs}: m.Vnode<HistogramAttrs>): m.Children {
    // Update cached attrs for callbacks (similar to React useRef pattern)
    this.currentAttrs = attrs;
    const {
      data,
      onFiltersChanged,
      height = DEFAULT_HEIGHT,
      xAxisLabel,
      yAxisLabel = 'Count',
      formatXValue = formatNumber,
      formatYValue = formatNumber,
      barColor,
      barHoverColor,
      logScale = false,
      fillParent = false,
      className,
    } = attrs;

    // Loading state
    if (data === undefined) {
      return this.renderLoading(height, fillParent, className);
    }

    // Empty state
    if (data.buckets.length === 0) {
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

    const maxCount = Math.max(...data.buckets.map((b) => b.count));
    const yScale = logScale
      ? d3
          .scaleLog()
          .domain([1, Math.max(maxCount, 1)])
          .range([chartHeight, 0])
      : d3.scaleLinear().domain([0, maxCount]).range([chartHeight, 0]).nice();

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
              this.handleBrush(start, end, data, this.currentAttrs);
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
        // Update scale on re-render (e.g., after filtering changes domain)
        this.brushHandler.updateScale(xScale);
      }
    }

    // Apply custom colors via CSS variables
    const style: Record<string, string> = {height: `${height}px`};
    if (barColor) style['--pf-d3-histogram-bar-color'] = barColor;
    if (barHoverColor) {
      style['--pf-d3-histogram-bar-hover-color'] = barHoverColor;
    }

    return m(
      '.pf-d3-histogram',
      {
        class: classNames(
          fillParent && 'pf-d3-histogram--fill-parent',
          className,
        ),
        style,
      },
      [
        m(
          'svg.pf-d3-histogram__svg',
          {
            viewBox: `0 0 ${VIEWBOX_WIDTH} ${height}`,
            preserveAspectRatio: 'xMidYMid meet',
            oncreate: (vnode) => {
              this.svgElement = vnode.dom as SVGSVGElement;
              // Force redraw to create BrushHandler now that SVG element exists
              m.redraw();
            },
          },
          [
            m(
              'g.pf-d3-histogram__chart-area',
              {
                transform: `translate(${DEFAULT_MARGIN.left}, ${DEFAULT_MARGIN.top})`,
                style: onFiltersChanged ? {cursor: 'crosshair'} : undefined,
                // Use stable handler references (created once in class constructor scope)
                onpointerdown: onFiltersChanged
                  ? this.handlePointerDown
                  : undefined,
                onpointermove: onFiltersChanged
                  ? this.handlePointerMove
                  : undefined,
                onpointerup: onFiltersChanged
                  ? this.handlePointerUp
                  : undefined,
                onpointercancel: onFiltersChanged
                  ? this.handlePointerCancel
                  : undefined,
              },
              [
                // Background for click capture
                m('rect.pf-d3-histogram__background', {
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

                // Bars
                ...this.renderBars(
                  data.buckets,
                  xScale,
                  yScale,
                  chartWidth,
                  chartHeight,
                  logScale,
                ),

                // Brush overlay (in-progress selection)
                this.renderBrushOverlay(xScale, chartHeight),

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
          ],
        ),

        // Tooltip
        this.hoveredBucket &&
          this.renderTooltip(this.hoveredBucket, formatXValue, formatYValue),
      ],
    );
  }

  private renderBars(
    buckets: readonly HistogramBucket[],
    _xScale: d3.ScaleLinear<number, number>,
    yScale:
      | d3.ScaleLinear<number, number>
      | d3.ScaleLogarithmic<number, number>,
    chartWidth: number,
    chartHeight: number,
    logScale: boolean,
  ): m.ChildArray {
    const bucketWidth = chartWidth / buckets.length;

    return buckets
      .map((bucket, i) => {
        // Skip zero-count buckets in log scale (log(0) is undefined)
        if (logScale && bucket.count === 0) return null;

        const x = i * bucketWidth;
        const y = yScale(bucket.count);
        const barHeight = chartHeight - y;
        const isHovered = this.hoveredBucket === bucket;

        return m('rect.pf-d3-histogram__bar', {
          x,
          y,
          width: Math.max(bucketWidth - 1, 1),
          height: barHeight,
          class: classNames(isHovered && 'pf-d3-histogram__bar--hover'),
          onmouseenter: () => {
            this.hoveredBucket = bucket;
          },
          onmouseleave: () => {
            this.hoveredBucket = undefined;
          },
        });
      })
      .filter((x): x is m.Vnode => x !== null);
  }

  private renderBrushOverlay(
    xScale: d3.ScaleLinear<number, number>,
    chartHeight: number,
  ): m.Children {
    const brush = this.brushHandler?.getCurrentBrush();
    if (!brush) return null;

    const startX = xScale(brush.start);
    const endX = xScale(brush.end);
    // Use Math.min to ensure x is always the left position,
    // and Math.abs to ensure width is always positive
    const x = Math.min(startX, endX);
    const width = Math.abs(endX - startX);

    return m('rect.pf-d3-histogram__brush-selection', {
      x,
      y: 0,
      width,
      height: chartHeight,
    });
  }

  private handleBrush(
    start: number,
    end: number,
    data: HistogramData,
    attrs: HistogramAttrs,
  ): void {
    if (!attrs.onFiltersChanged) {
      return;
    }

    // Check if this is a click on a specific bucket
    const clickedBucket = data.buckets.find(
      (b) => start >= b.start && start <= b.end && Math.abs(end - start) < 0.01,
    );

    if (clickedBucket && clickedBucket.count > 0) {
      // Click on non-empty bucket - filter to bucket boundaries
      const newFilters: Filter[] = [
        ...attrs.filters.filter((f) => f.field !== attrs.column),
        {field: attrs.column, op: '>=', value: clickedBucket.start},
        {field: attrs.column, op: '<=', value: clickedBucket.end},
      ];

      attrs.onFiltersChanged(newFilters);
    } else if (Math.abs(end - start) >= 0.01) {
      // Drag brush - filter to selected range
      const newFilters: Filter[] = [
        ...attrs.filters.filter((f) => f.field !== attrs.column),
        {field: attrs.column, op: '>=', value: start},
        {field: attrs.column, op: '<=', value: end},
      ];

      attrs.onFiltersChanged(newFilters);
    }
  }

  private handleClearFilters(attrs: HistogramAttrs): void {
    if (!attrs.onFiltersChanged) {
      return;
    }

    // Remove all filters for this column
    const newFilters = attrs.filters.filter((f) => f.field !== attrs.column);
    attrs.onFiltersChanged(newFilters);
  }

  private renderLoading(
    height: number,
    fillParent: boolean,
    className?: string,
  ): m.Children {
    return m(
      '.pf-d3-histogram',
      {
        class: classNames(
          fillParent && 'pf-d3-histogram--fill-parent',
          className,
        ),
        style: {height: `${height}px`},
      },
      m('.pf-d3-histogram__loading', m(Spinner)),
    );
  }

  private renderEmpty(
    height: number,
    fillParent: boolean,
    className?: string,
  ): m.Children {
    return m(
      '.pf-d3-histogram',
      {
        class: classNames(
          fillParent && 'pf-d3-histogram--fill-parent',
          className,
        ),
        style: {height: `${height}px`},
      },
      m('.pf-d3-histogram__empty', 'No data to display'),
    );
  }

  private renderTooltip(
    bucket: HistogramBucket,
    formatXValue: (value: number) => string,
    formatYValue: (value: number) => string,
  ): m.Children {
    return m(
      '.pf-d3-histogram__tooltip',
      m('.pf-d3-histogram__tooltip-content', [
        m(
          '.pf-d3-histogram__tooltip-row',
          `${formatXValue(bucket.start)} – ${formatXValue(bucket.end)}`,
        ),
        m('.pf-d3-histogram__tooltip-row', formatYValue(bucket.count)),
      ]),
    );
  }
}
