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
import {QueryNode, nextNodeId, NodeType, NodeContext} from '../../query_node';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import {isQuantitativeType} from '../../../../trace_processor/perfetto_sql_type';
import {SqlValue} from '../../../../trace_processor/query_result';
import {ChartAggregation} from '../../../../components/widgets/charts/chart_utils';
import {
  UIFilter,
  createAutoGroupedFiltersProto,
  formatFilterSummary,
  formatFilterValue,
  isFilterDefinitionValid,
  parseFilterValue,
} from '../operations/filter';
import {Chip} from '../../../../widgets/chip';
import {StructuredQueryBuilder} from '../structured_query_builder';
import {NodeIssues} from '../node_issues';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../../node_types';
import {NodeDetailsMessage, NodeTitle} from '../node_styling_widgets';
import {loadNodeDoc} from '../node_doc_loader';
import {AddItemPlaceholder} from '../widgets';
import {Button} from '../../../../widgets/button';
import {Icon} from '../../../../widgets/icon';
import {Card} from '../../../../widgets/card';
import {classNames} from '../../../../base/classnames';
import {renderChartTypePickerGrid} from '../charts/chart_type_picker';
import {Popup} from '../../../../widgets/popup';

/**
 * Chart type options.
 */
export type ChartType =
  | 'bar'
  | 'histogram'
  | 'line'
  | 'scatter'
  | 'pie'
  | 'treemap'
  | 'boxplot'
  | 'heatmap'
  | 'cdf'
  | 'scorecard';

/**
 * Bar chart orientation options.
 */
export type BarOrientation = 'horizontal' | 'vertical';

/**
 * Configuration for a single chart within a visualisation node.
 *
 * ## Current Single-Measure Limitation
 *
 * The current implementation supports a single measure per chart. This means:
 * - Bar charts can only show one aggregation at a time (e.g., COUNT or SUM(duration))
 * - Histograms visualize a single numeric column's distribution
 *
 * ## Future Multi-Measure Support
 *
 * To extend this to support multiple measures (e.g., overlay multiple metrics):
 *
 * 1. Replace `measureColumn` and `aggregation` with an array:
 *    ```typescript
 *    measures?: Array<{
 *      column: string;
 *      aggregation: ChartAggregation;
 *      color?: string;
 *    }>;
 *    ```
 *
 * 2. Update chart_data_loader.ts to fetch multiple aggregations in one query
 *
 * 3. Update bar_chart.ts to render multiple bars per category (grouped/stacked)
 *
 * 4. Update the chart config UI to allow adding/removing measures
 *
 * The registry pattern in chart_type_registry.ts can help determine which chart
 * types support multiple measures via a new `supportsMultipleMeasures` flag.
 */
export interface ChartConfig {
  /** Unique identifier for this chart */
  readonly id: string;
  /** Custom display name for this chart (optional, defaults to column name) */
  name?: string;
  /** Column to visualize (dimension for bar charts, value for histograms) */
  column: string;
  /** Type of chart - see chart_type_registry.ts for available types */
  chartType: ChartType;
  /** Number of bins for histogram (optional, auto-calculated if not set) */
  binCount?: number;
  /** Bar chart orientation (default: horizontal) */
  orientation?: BarOrientation;
  /** Custom width in pixels (optional, uses flex layout if not set) */
  widthPx?: number;
  /**
   * Aggregation function for bar/pie/treemap charts (default: COUNT).
   * Currently only a single aggregation is supported per chart.
   * See the interface docs above for multi-measure extension plans.
   */
  aggregation?: ChartAggregation;
  /**
   * Measure/size column for non-count aggregations (bar, pie) or always for
   * treemap. Currently only a single measure column is supported.
   */
  measureColumn?: string;
  /**
   * Y-axis column for line and scatter charts (numeric).
   * Required for those chart types.
   */
  yColumn?: string;
  /**
   * Optional grouping column: series grouping for line/scatter,
   * parent grouping for treemap.
   */
  groupColumn?: string;
  /**
   * Optional bubble-size column for scatter charts (numeric).
   */
  sizeColumn?: string;
}

/**
 * Generate a unique ID for a new chart.
 */
export function generateChartId(): string {
  return `chart-${crypto.randomUUID()}`;
}

/**
 * Generate a default display label for a chart config.
 * Used by both the node card and the chart view header.
 */
export function getDefaultChartLabel(config: ChartConfig): string {
  if (!config.column) return 'Not configured';
  switch (config.chartType) {
    case 'histogram':
      return `Histogram: ${config.column}`;
    case 'line':
      return config.yColumn
        ? `${config.yColumn} vs ${config.column}`
        : `Line: ${config.column}`;
    case 'scatter':
      return config.yColumn
        ? `${config.yColumn} vs ${config.column}`
        : `Scatter: ${config.column}`;
    case 'boxplot':
      return config.measureColumn
        ? `${config.measureColumn} by ${config.column}`
        : `Boxplot: ${config.column}`;
    case 'heatmap':
      return config.yColumn
        ? `${config.column} vs ${config.yColumn}`
        : `Heatmap: ${config.column}`;
    case 'cdf':
      return `CDF: ${config.column}`;
    case 'scorecard': {
      const agg = config.aggregation ?? 'COUNT_DISTINCT';
      return `${agg}(${config.measureColumn ?? config.column})`;
    }
    case 'pie':
    case 'treemap':
    case 'bar': {
      const agg = config.aggregation ?? 'COUNT';
      if (agg === 'COUNT') return `Count by ${config.column}`;
      return `${agg}(${config.measureColumn ?? config.column}) by ${config.column}`;
    }
  }
}

// Serializable node configuration.
export interface VisualisationNodeAttrs {
  /** Array of chart configurations - multiple charts per node */
  chartConfigs: ChartConfig[];
  /** Shared filters applied to all charts */
  chartFilters?: Partial<UIFilter>[];
}

export class VisualisationNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kVisualisation;
  primaryInput?: QueryNode;
  secondaryInputs?: undefined;
  nextNodes: QueryNode[];
  readonly attrs: VisualisationNodeAttrs;
  readonly context: NodeContext;

  // UI-only state for tracking collapsed charts (not persisted)
  private collapsedCharts: Set<string> = new Set();
  // UI-only state for tracking which chart title is being edited
  private editingChartId?: string;

  constructor(attrs: VisualisationNodeAttrs, context: NodeContext) {
    this.nodeId = nextNodeId();
    // Migrate filter values from strings back to numbers when appropriate.
    // JSON serialization converts BigInt to string, and we need numeric
    // values for proper filtering (same logic as FilterNode).
    const chartFilters = attrs.chartFilters?.map((f): Partial<UIFilter> => {
      if ('value' in f && typeof f.value === 'string') {
        if (!Array.isArray(f.value)) {
          const parsed = parseFilterValue(f.value);
          if (parsed !== undefined && parsed !== f.value) {
            return {...f, value: parsed} as Partial<UIFilter>;
          }
        }
      }
      return f;
    });
    this.attrs = {
      ...attrs,
      chartConfigs: attrs.chartConfigs ?? [],
      chartFilters: chartFilters ?? [],
    };
    this.context = context;
    this.nextNodes = [];
  }

  get sourceCols(): ColumnInfo[] {
    return this.primaryInput?.finalCols ?? [];
  }

  get finalCols(): ColumnInfo[] {
    // Visualisation node doesn't change the schema - it's a visualization/filter node
    return this.sourceCols;
  }

  /**
   * Get columns suitable for the primary column of a given chart type.
   * Histogram, line, and scatter require numeric columns for their primary
   * axis; all other types accept any column.
   */
  getChartableColumns(chartType: ChartType): ColumnInfo[] {
    if (
      chartType === 'histogram' ||
      chartType === 'line' ||
      chartType === 'scatter' ||
      chartType === 'cdf'
    ) {
      return this.sourceCols.filter((col) => {
        const type = col.type;
        return type !== undefined && isQuantitativeType(type);
      });
    }
    return this.sourceCols;
  }

  /**
   * Check if a filter is valid for this node.
   */
  private isFilterValid(filter: Partial<UIFilter>): filter is UIFilter {
    if (!isFilterDefinitionValid(filter)) {
      return false;
    }
    const columnExists = this.sourceCols.some(
      (col) => col.name === filter.column,
    );
    return columnExists;
  }

  getTitle(): string {
    return 'Visualisation';
  }

  /** Number of charts that have a column selected. */
  private configuredChartCount(): number {
    return this.attrs.chartConfigs.filter((c) => c.column).length;
  }

  private setValidationError(message: string): void {
    if (!this.context.issues) {
      this.context.issues = new NodeIssues();
    }
    this.context.issues.queryError = new Error(message);
  }

  nodeDetails(): NodeDetailsAttrs {
    this.validate();

    const validFilters =
      this.attrs.chartFilters?.filter((f) => this.isFilterValid(f)) ?? [];

    const configuredCount = this.configuredChartCount();
    if (configuredCount === 0) {
      return {
        content: [
          NodeTitle(this.getTitle()),
          NodeDetailsMessage('No charts configured'),
        ],
      };
    }

    if (validFilters.length === 0) {
      return {
        content: [
          NodeTitle(this.getTitle()),
          m(
            '.pf-visualisation-node-details',
            `${configuredCount} chart${configuredCount !== 1 ? 's' : ''}`,
          ),
        ],
      };
    }

    return {
      content: [
        NodeTitle(this.getTitle()),
        m('.pf-visualisation-node-details', [
          m(
            '.pf-visualisation-node-details__config',
            `${configuredCount} chart${configuredCount !== 1 ? 's' : ''}`,
          ),
          formatFilterSummary(validFilters),
        ]),
      ],
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    this.validate();

    // Build sections
    const sections: NodeModifyAttrs['sections'] = [];

    // Chart cards container
    const chartCards = this.attrs.chartConfigs.map((config) => {
      const chartTypeIcon =
        config.chartType === 'bar' ? 'bar_chart' : 'ssid_chart';
      const defaultLabel = getDefaultChartLabel(config);

      // Filters matching this chart's column
      const chartFilters = (this.attrs.chartFilters?.filter(
        (f) => this.isFilterValid(f) && f.column === config.column,
      ) ?? []) as UIFilter[];
      const hasFilters = chartFilters.length > 0;

      // Collapse by default when no filters
      const isCollapsed = hasFilters
        ? this.collapsedCharts.has(config.id)
        : true;

      return m(
        Card,
        {
          key: config.id,
          className: classNames(
            'pf-chart-card',
            isCollapsed && 'pf-chart-card--collapsed',
          ),
        },
        [
          // Card header (always visible)
          m('.pf-chart-card__header', [
            // Toggle button (only when there are filters to show)
            hasFilters
              ? m(
                  '.pf-chart-card__toggle',
                  {
                    onclick: () => {
                      this.toggleChartCollapsed(config.id);
                    },
                  },
                  m(Icon, {
                    icon: isCollapsed ? 'chevron_right' : 'expand_more',
                  }),
                )
              : m('.pf-chart-card__toggle'),
            // Chart info
            m('.pf-chart-card__info', [
              m(Icon, {icon: chartTypeIcon}),
              this.editingChartId === config.id
                ? m('input.pf-chart-card__title-input', {
                    type: 'text',
                    value: config.name ?? '',
                    placeholder: defaultLabel,
                    oncreate: (vnode: m.VnodeDOM) => {
                      const input = vnode.dom as HTMLInputElement;
                      input.focus();
                      input.select();
                    },
                    onblur: (e: Event) => {
                      // Trim the value when user finishes editing
                      const target = e.target as HTMLInputElement;
                      const name = target.value.trim() || undefined;
                      this.updateChart(config.id, {name});
                      this.editingChartId = undefined;
                    },
                    onkeydown: (e: KeyboardEvent) => {
                      if (e.key === 'Enter' || e.key === 'Escape') {
                        e.stopPropagation();
                        this.editingChartId = undefined;
                        (e.target as HTMLInputElement).blur();
                      }
                    },
                    oninput: (e: Event) => {
                      const target = e.target as HTMLInputElement;
                      // Don't trim while typing - let the user type spaces!
                      const name = target.value || undefined;
                      this.updateChart(config.id, {name});
                    },
                    onclick: (e: Event) => {
                      e.stopPropagation();
                    },
                  })
                : m(
                    '.pf-chart-card__title',
                    {
                      onclick: (e: Event) => {
                        e.stopPropagation();
                        this.editingChartId = config.id;
                      },
                      title: 'Click to rename',
                    },
                    config.name ?? defaultLabel,
                  ),
            ]),
            // Actions
            m('.pf-chart-card__actions', [
              hasFilters &&
                m(
                  '.pf-chart-card__filter-count',
                  {title: `${chartFilters.length} active filter(s)`},
                  [
                    `${chartFilters.length}`,
                    m(
                      '.pf-chart-card__filter-count-close',
                      {
                        onclick: (e: Event) => {
                          e.stopPropagation();
                          this.clearChartFiltersForColumn(config.column);
                          this.context.onchange?.();
                        },
                        title: 'Clear filters for this chart',
                      },
                      '\u00d7',
                    ),
                  ],
                ),
              // Remove button (only show if more than one chart)
              this.attrs.chartConfigs.length > 1 &&
                m(Button, {
                  icon: 'close',
                  compact: true,
                  title: 'Remove chart',
                  onclick: (e: Event) => {
                    e.stopPropagation();
                    this.removeChart(config.id);
                  },
                }),
            ]),
          ]),
          // Card body — individual filter chips
          !isCollapsed &&
            hasFilters &&
            m('.pf-chart-card__body', [
              m(
                '.pf-chart-card__filter-label',
                `Filters on `,
                m('strong', config.column),
              ),
              m(
                '.pf-chart-card__filters',
                chartFilters.map((filter, i) =>
                  m(Chip, {
                    key: `filter-${i}`,
                    label: formatFilterValue(filter),
                    compact: true,
                    rounded: true,
                    removable: true,
                    onRemove: () => {
                      // Find the index in the full filters array
                      const fullIndex =
                        this.attrs.chartFilters?.indexOf(filter) ?? -1;
                      if (fullIndex >= 0) {
                        this.removeChartFilter(fullIndex);
                      }
                    },
                    removeButtonTitle: 'Remove filter',
                  }),
                ),
              ),
            ]),
        ],
      );
    });

    // Charts section with add button and cards
    sections.push({
      title: 'Charts',
      content: m('.pf-chart-cards-container', [
        ...chartCards,
        m(
          Popup,
          {
            key: 'add-chart',
            trigger: m(
              'span',
              m(AddItemPlaceholder, {
                label: 'Add Chart',
                icon: 'add',
              }),
            ),
            fitContent: true,
          },
          renderChartTypePickerGrid((chartType: ChartType) => {
            this.addChart(chartType);
          }),
        ),
      ]),
    });

    const info =
      'Visualize your data with charts. Click on chart elements to add filters that affect all charts.';

    return {
      info,
      sections,
    };
  }

  /**
   * Toggle the collapsed state of a chart card.
   */
  private toggleChartCollapsed(chartId: string): void {
    if (this.collapsedCharts.has(chartId)) {
      this.collapsedCharts.delete(chartId);
    } else {
      this.collapsedCharts.add(chartId);
    }
    // No explicit m.redraw() needed — this is always called from an onclick
    // handler, which triggers Mithril's automatic redraw.
  }

  /**
   * Add a new chart configuration with a sensible default column.
   * Prefers string/categorical columns for bar charts, and avoids
   * columns already used by other charts when possible.
   */
  addChart(chartType: ChartType = 'bar'): void {
    // Get columns already used by existing charts
    const usedColumns = new Set(
      this.attrs.chartConfigs.map((c) => c.column).filter(Boolean),
    );

    // Find a good default column:
    // 1. Prefer unused columns
    // 2. Prefer non-numeric columns (better for aggregation chart types)
    // 3. Fall back to first available column
    const availableCols = this.sourceCols;
    const unusedCols = availableCols.filter((c) => !usedColumns.has(c.name));
    const colsToCheck = unusedCols.length > 0 ? unusedCols : availableCols;

    // Prefer string/categorical columns for aggregation charts
    const stringCol = colsToCheck.find(
      (c) => c.type === undefined || !isQuantitativeType(c.type),
    );
    const defaultColumn = stringCol?.name ?? colsToCheck[0]?.name ?? '';

    const newChart: ChartConfig = {
      id: generateChartId(),
      column: defaultColumn,
      chartType,
    };
    this.attrs.chartConfigs.push(newChart);
    this.context.onchange?.();
  }

  /**
   * Update a chart configuration by ID.
   */
  updateChart(
    chartId: string,
    updates: Partial<Omit<ChartConfig, 'id'>>,
  ): void {
    const chartIndex = this.attrs.chartConfigs.findIndex(
      (c) => c.id === chartId,
    );
    if (chartIndex === -1) return;

    const config = this.attrs.chartConfigs[chartIndex];
    this.attrs.chartConfigs[chartIndex] = {
      ...config,
      ...updates,
    };
    this.context.onchange?.();
  }

  /**
   * Remove a chart configuration by ID.
   */
  removeChart(chartId: string): void {
    this.attrs.chartConfigs = this.attrs.chartConfigs.filter(
      (c) => c.id !== chartId,
    );
    this.context.onchange?.();
  }

  /**
   * Duplicate a chart configuration by ID.
   * Creates a copy with a new ID placed after the original.
   */
  duplicateChart(chartId: string): void {
    const chartIndex = this.attrs.chartConfigs.findIndex(
      (c) => c.id === chartId,
    );
    if (chartIndex === -1) return;

    const original = this.attrs.chartConfigs[chartIndex];
    const duplicate: ChartConfig = {
      ...original,
      id: generateChartId(),
      name: original.name ? `${original.name} (copy)` : undefined,
    };

    // Insert after the original
    this.attrs.chartConfigs.splice(chartIndex + 1, 0, duplicate);
    this.context.onchange?.();
  }

  /**
   * Reorder a chart by moving it before another chart.
   * @param draggedId The ID of the chart being dragged
   * @param targetId The ID of the chart to drop before
   */
  reorderChart(draggedId: string, targetId: string): void {
    const configs = this.attrs.chartConfigs;
    const draggedIndex = configs.findIndex((c) => c.id === draggedId);
    const targetIndex = configs.findIndex((c) => c.id === targetId);

    if (draggedIndex === -1 || targetIndex === -1) return;
    if (draggedIndex === targetIndex) return;

    // Remove the dragged item
    const [draggedItem] = configs.splice(draggedIndex, 1);

    // Calculate new target index (may have shifted after removal)
    const newTargetIndex =
      draggedIndex < targetIndex ? targetIndex - 1 : targetIndex;

    // Insert at new position
    configs.splice(newTargetIndex, 0, draggedItem);

    this.context.onchange?.();
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('visualisation');
  }

  validate(): boolean {
    if (this.context.issues) {
      this.context.issues.clear();
    }

    if (this.primaryInput === undefined) {
      this.setValidationError('No input node connected');
      return false;
    }

    if (!this.primaryInput.validate()) {
      this.setValidationError('Previous node is invalid');
      return false;
    }

    if (this.sourceCols.length === 0) {
      this.setValidationError(
        'No columns available. Please connect a data source.',
      );
      return false;
    }

    return true;
  }

  clone(): QueryNode {
    return new VisualisationNode(
      {
        chartConfigs: this.attrs.chartConfigs.map((c) => ({...c})),
        chartFilters: this.attrs.chartFilters?.map((f) => ({...f})),
      },
      this.context,
    );
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (this.primaryInput === undefined) return undefined;

    // Get valid filters
    const validFilters =
      this.attrs.chartFilters?.filter((f) => this.isFilterValid(f)) ?? [];

    if (validFilters.length === 0) {
      // No filters - return passthrough
      return StructuredQueryBuilder.passthrough(this.primaryInput, this.nodeId);
    }

    const filtersProto = createAutoGroupedFiltersProto(
      validFilters,
      this.sourceCols,
    );

    if (filtersProto === undefined) {
      return StructuredQueryBuilder.passthrough(this.primaryInput, this.nodeId);
    }

    return StructuredQueryBuilder.withFilter(
      this.primaryInput,
      filtersProto,
      this.nodeId,
    );
  }

  /**
   * Add a filter from a chart click.
   * This is called by the chart component when a user clicks on a bar/bin.
   */
  addChartFilter(filter: UIFilter): void {
    if (!this.attrs.chartFilters) {
      this.attrs.chartFilters = [];
    }
    this.attrs.chartFilters.push({...filter, enabled: true});
    this.context.onchange?.();
  }

  /**
   * Add a range filter (for histogram bins).
   * Creates two filters: column >= min AND column < max
   */
  addRangeFilter(column: string, min: number, max: number): void {
    if (!this.attrs.chartFilters) {
      this.attrs.chartFilters = [];
    }
    // Add >= min filter
    this.attrs.chartFilters.push({
      column,
      op: '>=',
      value: min,
      enabled: true,
    });
    // Add < max filter
    this.attrs.chartFilters.push({
      column,
      op: '<',
      value: max,
      enabled: true,
    });
    this.context.onchange?.();
  }

  /**
   * Clear all chart filters.
   */
  clearChartFilters(): void {
    this.attrs.chartFilters = [];
    this.context.onchange?.();
  }

  /**
   * Clear chart filters for a specific column only.
   * Keeps filters on other columns intact.
   *
   * Does NOT call `onchange`. Use this when immediately followed by a method
   * that adds replacement filters and calls `onchange` (e.g. `setBrushSelection`,
   * `addRangeFilter`). If clearing filters is the final operation, callers must
   * call `this.context.onchange?.()` themselves — or use `clearChartFilters()`
   * which handles it automatically.
   */
  clearChartFiltersForColumn(column: string): void {
    if (!this.attrs.chartFilters) return;
    this.attrs.chartFilters = this.attrs.chartFilters.filter(
      (f) => f.column !== column,
    );
  }

  /**
   * Toggle the enabled state of a chart filter by index.
   * Calls onchange to rebuild query so child nodes see filtered data.
   */
  toggleChartFilter(index: number): void {
    if (!this.attrs.chartFilters || index >= this.attrs.chartFilters.length) {
      return;
    }
    const filter = this.attrs.chartFilters[index];
    this.attrs.chartFilters[index] = {
      ...filter,
      enabled: !(filter.enabled ?? true),
    };
    this.context.onchange?.();
  }

  /**
   * Remove a chart filter by index.
   * Calls onchange to rebuild query so child nodes see filtered data.
   */
  removeChartFilter(index: number): void {
    if (!this.attrs.chartFilters || index >= this.attrs.chartFilters.length) {
      return;
    }
    this.attrs.chartFilters = this.attrs.chartFilters.filter(
      (_, i) => i !== index,
    );
    this.context.onchange?.();
  }

  /**
   * Set brush selection filters (multiple values with OR logic).
   * Used when user drags to select multiple bars in a bar chart.
   * Note: Caller should call clearChartFiltersForColumn() first if needed.
   */
  setBrushSelection(column: string, values: SqlValue[]): void {
    if (!this.attrs.chartFilters) {
      this.attrs.chartFilters = [];
    }
    for (const value of values) {
      if (value === null) {
        this.attrs.chartFilters.push({
          column,
          op: 'is null',
          enabled: true,
        });
      } else {
        this.attrs.chartFilters.push({
          column,
          op: '=',
          value,
          enabled: true,
        });
      }
    }

    this.context.onchange?.();
  }
}
