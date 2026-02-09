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
 * Stacked bar chart component (multi-series, vertically stacked bars).
 *
 * Displays multiple series as stacked segments within each bar, supporting:
 * - Multi-category brush selection (drag across categories)
 * - Click anywhere to clear filters
 * - Hover tooltips showing group and value
 * - Sorting by category or total stacked value
 * - Legend for groups
 *
 * State management: Always controlled by parent.
 * Parent owns filters array, chart emits new array on interaction.
 *
 * @example
 * ```typescript
 * m(StackedBarChart, {
 *   data: {
 *     bars: [
 *       {category: 'Q1', value: 100, group: 'Revenue'},
 *       {category: 'Q1', value: 30, group: 'Profit'},
 *       {category: 'Q2', value: 120, group: 'Revenue'},
 *       {category: 'Q2', value: 40, group: 'Profit'},
 *     ],
 *     groups: ['Revenue', 'Profit'],
 *   },
 *   filters: this.filters,
 *   column: 'quarter',
 *   onFiltersChanged: (filters) => { this.filters = filters; },
 * })
 * ```
 */

import m from 'mithril';
import * as d3 from 'd3';
import {classNames} from '../../../../base/classnames';
import {Spinner} from '../../../../widgets/spinner';
import {
  DEFAULT_MARGIN,
  VIEWBOX_WIDTH,
  LEGEND_WIDTH,
  formatNumber,
  createCategoricalColorScale,
} from '../chart_utils';
import {
  renderLinearAxis,
  renderBandAxis,
  renderGridLines,
} from '../renderers/axis_renderer';
import {renderLegend} from '../renderers/legend_renderer';
import {BrushHandlerCategorical} from '../interactions/brush_handler';
import {BarDatum, SortConfig, GroupedBarData} from './bar_types';
import {Filter} from '../../../../components/widgets/datagrid/model';
import {
  extractCategories,
  extractGroups,
  sortBars,
  getSelectedCategories,
  createFiltersWithCategories,
  toggleCategoryFilter,
  clearColumnFilters,
  computeStackedLayout,
  getMaxStackedValue,
} from './bar_utils';

const DEFAULT_HEIGHT = 200;

/**
 * Attributes for stacked bar chart (multi-series, stacked).
 */
export interface StackedBarChartAttrs {
  readonly data: GroupedBarData | undefined;
  readonly filters: readonly Filter[];
  readonly column: string;
  readonly onFiltersChanged?: (filters: readonly Filter[]) => void;
  readonly height?: number;
  readonly xAxisLabel?: string;
  readonly yAxisLabel?: string;
  readonly fillParent?: boolean;
  readonly className?: string;
  readonly formatXValue?: (value: string) => string;
  readonly formatYValue?: (value: number) => string;
  readonly sort?: SortConfig;
  readonly colors?: readonly string[];
  readonly showLegend?: boolean;
}

/**
 * Stacked bar chart component.
 * Zero anti-patterns: No justBrushed, no internalFilters, always controlled.
 */
export class StackedBarChart implements m.ClassComponent<StackedBarChartAttrs> {
  private svgElement?: SVGSVGElement;
  private brushHandler?: BrushHandlerCategorical;
  private hoveredBar?: BarDatum;
  // Cache current props for callback access (like React useRef)
  private currentFilters: readonly Filter[] = [];
  private currentColumn: string = '';

  view({attrs}: m.Vnode<StackedBarChartAttrs>) {
    const {
      data,
      filters,
      column,
      onFiltersChanged,
      height = DEFAULT_HEIGHT,
      xAxisLabel,
      yAxisLabel,
      fillParent,
      className,
      formatXValue = (v) => v,
      formatYValue = (v) => formatNumber(v),
      sort,
      colors,
      showLegend = true,
    } = attrs;

    const hasActiveFilter = getSelectedCategories(filters, column).size > 0;

    if (data === undefined) {
      return m(
        '.pf-d3-bar',
        {
          className: classNames(
            fillParent && 'pf-d3-bar--fill-parent',
            className,
          ),
          style: {height: `${height}px`},
        },
        m('.pf-d3-bar__loading', m(Spinner)),
      );
    }

    if (data.bars.length === 0) {
      return m(
        '.pf-d3-bar',
        {
          className: classNames(
            fillParent && 'pf-d3-bar--fill-parent',
            className,
          ),
          style: {height: `${height}px`},
        },
        m('.pf-d3-bar__empty', 'No data to display'),
      );
    }

    // Apply sorting if requested
    const sortedBars = sort ? sortBars(data.bars, sort, true) : data.bars;
    const categories = extractCategories(sortedBars);
    const groups =
      data.groups.length > 0 ? data.groups : extractGroups(sortedBars);
    const selectedCategories = getSelectedCategories(filters, column);

    // Update cached props for callbacks (similar to React useRef pattern)
    this.currentFilters = filters;
    this.currentColumn = column;

    // Compute stacked layout
    const stackedLayout = computeStackedLayout(sortedBars, categories, groups);
    const maxStackedValue = getMaxStackedValue(sortedBars, categories);

    // Create color scale
    const colorScale = createCategoricalColorScale(
      [...groups],
      Array.isArray(colors) ? colors : undefined,
    );

    const chartWidth =
      VIEWBOX_WIDTH - DEFAULT_MARGIN.left - DEFAULT_MARGIN.right;
    const chartHeight = height - DEFAULT_MARGIN.top - DEFAULT_MARGIN.bottom;

    // Create scales
    const xScale = d3
      .scaleBand<string>()
      .domain(categories)
      .range([0, chartWidth])
      .padding(0.2);

    const yScale = d3
      .scaleLinear()
      .domain([0, maxStackedValue])
      .range([chartHeight, 0])
      .nice();

    // Initialize or update brush handler
    if (onFiltersChanged && this.svgElement) {
      if (!this.brushHandler) {
        this.brushHandler = new BrushHandlerCategorical(
          this.svgElement,
          xScale,
          DEFAULT_MARGIN,
          categories,
          (selectedCategories) => {
            // Access current props via instance (avoids stale closure)
            const newFilters = createFiltersWithCategories(
              this.currentFilters,
              this.currentColumn,
              selectedCategories,
            );
            onFiltersChanged(newFilters);
          },
          (category) => {
            // Access current props via instance (avoids stale closure)
            const currentSelected = getSelectedCategories(
              this.currentFilters,
              this.currentColumn,
            );
            if (currentSelected.size > 1) {
              // Multiple selected: replace with only this category
              const newFilters = createFiltersWithCategories(
                this.currentFilters,
                this.currentColumn,
                [category],
              );
              onFiltersChanged(newFilters);
            } else {
              // 0 or 1 selected: toggle behavior
              const newFilters = toggleCategoryFilter(
                this.currentFilters,
                this.currentColumn,
                category,
              );
              onFiltersChanged(newFilters);
            }
          },
          () => {
            const newFilters = clearColumnFilters(
              this.currentFilters,
              this.currentColumn,
            );
            onFiltersChanged(newFilters);
          },
        );
      } else {
        // Update scale and categories on re-render (e.g., after filtering changes domain)
        this.brushHandler.updateScaleAndCategories(xScale, categories);
      }
    }

    return m(
      '.pf-d3-bar.pf-d3-bar--stacked',
      {
        className: classNames(
          fillParent && 'pf-d3-bar--fill-parent',
          hasActiveFilter && 'pf-d3-bar--active-filter',
          className,
        ),
      },
      [
        m(
          'svg.pf-d3-bar__svg',
          {
            viewBox: `0 0 ${showLegend && groups.length > 1 ? VIEWBOX_WIDTH + LEGEND_WIDTH : VIEWBOX_WIDTH} ${height}`,
            preserveAspectRatio: 'xMidYMid meet',
            style: {height: `${height}px`},
            oncreate: (vnode: m.VnodeDOM) => {
              this.svgElement = vnode.dom as SVGSVGElement;
            },
          },
          [
            m(
              'g.pf-d3-bar__chart-area',
              {
                transform: `translate(${DEFAULT_MARGIN.left}, ${DEFAULT_MARGIN.top})`,
                ...(this.brushHandler?.getEventHandlers() ?? {}),
              },
              [
                // Background for click capture
                m('rect.pf-d3-bar__background', {
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

                // Stacked bars
                ...this.renderStackedBars(
                  sortedBars,
                  xScale,
                  yScale,
                  stackedLayout,
                  colorScale,
                  selectedCategories,
                ),

                // Brush selection overlay
                this.renderBrushOverlay(xScale, chartHeight),

                // Y-axis (left)
                renderLinearAxis({
                  scale: yScale,
                  orientation: 'left',
                  length: chartHeight,
                  label: yAxisLabel,
                  tickFormatter: formatYValue,
                }),

                // X-axis (bottom)
                m(
                  'g',
                  {transform: `translate(0, ${chartHeight})`},
                  renderBandAxis({
                    scale: xScale,
                    orientation: 'bottom',
                    length: chartWidth,
                    label: xAxisLabel,
                    tickFormatter: formatXValue,
                  }),
                ),
              ],
            ),

            // Legend - in right margin
            showLegend &&
              m(
                'g',
                {
                  transform: `translate(${VIEWBOX_WIDTH + 100}, ${DEFAULT_MARGIN.top})`,
                },
                renderLegend({
                  items: groups.map((group) => ({
                    name: group,
                    color: colorScale(group),
                  })),
                  position: 'top-right',
                  chartWidth: 0,
                }),
              ),
          ],
        ),

        // Tooltip
        this.hoveredBar &&
          this.renderTooltip(this.hoveredBar, formatXValue, formatYValue),
      ],
    );
  }

  private renderStackedBars(
    bars: readonly BarDatum[],
    xScale: d3.ScaleBand<string>,
    yScale: d3.ScaleLinear<number, number>,
    stackedLayout: Map<string, Map<string, {y0: number; y1: number}>>,
    colorScale: (group: string) => string,
    selectedCategories: Set<string>,
  ): m.ChildArray {
    const rendered = bars
      .map((bar) => {
        const x = xScale(bar.category) ?? 0;
        const barWidth = xScale.bandwidth();

        // Get stacked position for this bar
        const categoryLayout = stackedLayout.get(bar.category);
        const segment = categoryLayout?.get(bar.group ?? '');

        if (!segment) {
          return null;
        }

        const y = yScale(segment.y1);
        const barHeight = yScale(segment.y0) - y;

        const isSelected = selectedCategories.has(bar.category);
        const isHovered = this.hoveredBar === bar;
        const color = colorScale(bar.group ?? '');

        return m('rect.pf-d3-bar__bar', {
          x,
          y,
          width: barWidth,
          height: barHeight,
          fill: color,
          class: classNames(
            isSelected && 'pf-d3-bar__bar--selected',
            isHovered && 'pf-d3-bar__bar--hover',
          ),
          onmouseenter: () => {
            this.hoveredBar = bar;
          },
          onmouseleave: () => {
            this.hoveredBar = undefined;
          },
        });
      })
      .filter((x): x is m.Vnode => x !== null);

    return rendered;
  }

  private renderBrushOverlay(
    xScale: d3.ScaleBand<string>,
    chartHeight: number,
  ): m.Children {
    const brushCategories = this.brushHandler?.getCurrentBrush();
    if (!brushCategories || brushCategories.length === 0) return null;

    const firstCategory = brushCategories[0];
    const lastCategory = brushCategories[brushCategories.length - 1];
    const startX = xScale(firstCategory) ?? 0;
    const endX = (xScale(lastCategory) ?? 0) + xScale.bandwidth();

    return m('rect.pf-d3-bar__brush-selection', {
      x: startX,
      y: 0,
      width: endX - startX,
      height: chartHeight,
    });
  }

  private renderTooltip(
    bar: BarDatum,
    formatXValue: (value: string) => string,
    formatYValue: (value: number) => string,
  ): m.Children {
    return m(
      '.pf-d3-bar__tooltip',
      m('.pf-d3-bar__tooltip-content', [
        m('.pf-d3-bar__tooltip-row', formatXValue(bar.category)),
        m('.pf-d3-bar__tooltip-row', bar.group ?? ''),
        m('.pf-d3-bar__tooltip-row', formatYValue(bar.value)),
      ]),
    );
  }
}
