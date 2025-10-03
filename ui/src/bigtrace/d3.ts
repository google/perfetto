import m from 'mithril';
import * as d3 from 'd3';
import {DataGrid} from '../components/widgets/data_grid/data_grid';
import {
  ColumnDefinition,
  DataGridDataSource,
  DataGridFilter,
} from '../components/widgets/data_grid/common';
import {DataGridAttrs} from '../components/widgets/data_grid/data_grid';

function truncate(str: string, maxLength: number): string {
  if (str.length <= maxLength) {
    return str;
  }
  return str.slice(0, maxLength) + '…';
}

// Add the global window update source flag that's in the working version
const storedValue = localStorage.getItem('updateSourceOnFilter');
(window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER = storedValue === null ? true : storedValue === 'true';

(window as any).setUpdateSourceOnFilter = function(value: boolean) {
  (window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER = value;
  localStorage.setItem('updateSourceOnFilter', value.toString());
};

class ChartManager {
  static charts: any[] = [];

  static register(chartComponent: any): any {
    if (!this.charts.includes(chartComponent)) {
      this.charts.push(chartComponent);
    }
    return chartComponent;
  }

  static unregister(chartComponent: any): void {
    const index = this.charts.indexOf(chartComponent);
    if (index > -1) {
      this.charts.splice(index, 1);
    }
  }

  static removeChart(chartDomElement: HTMLElement | null): void {
    if (!chartDomElement) return;
    const chartContainer = chartDomElement.closest('.chart-container');
    if (chartContainer) {
      const chartComponent = this.charts.find(c => c.dom === chartDomElement);
      if (chartComponent) {
        this.unregister(chartComponent);
      }
      chartContainer.remove();
      m.redraw();
    }
  }

  static duplicateChart(chartDomElement: HTMLElement | null): void {
    if (!chartDomElement) return;

    const chartComponent = this.charts.find(c => c.dom === chartDomElement);
    if (!chartComponent) return;

    if (chartComponent instanceof DataTableComponent) {
      (window as any).populateChartCreator?.('table', {});
      return;
    }

    if (!chartComponent.chart) return;

    const chart = chartComponent.chart;

    let chartType = '';
    let config: any = {};

    if (chart instanceof D3BarChart) {
      chartType = 'bar';
      config = {
        xColumn: chart.xColumnName,
        yColumn: chart.yColumnName,
        aggregation: chart.aggregationFunction,
      };
    } else if (chart instanceof D3HistogramChart) {
      chartType = 'histogram';
      config = { xColumn: chart.columnName };
    } else if (chart instanceof D3CDFChart) {
      chartType = 'cdf';
      config = {
        xColumn: chart.columnName,
        colorBy: chart.colorBy || '',
      };
    } else if (chart instanceof D3ScatterChart) {
      chartType = 'scatter';
      config = {
        xColumn: chart.xColumnName,
        yColumn: chart.yColumnName,
        colorBy: chart.colorBy || '',
      };
    } else if (chart instanceof D3HeatmapChart) {
      chartType = 'heatmap';
      config = {
        xColumn: chart.xColumnName,
        yColumn: chart.yColumnName,
        valueColumn: chart.valueColumnName,
        aggregation: chart.aggregationFunction,
      };
    } else if (chart instanceof D3BoxplotChart) {
      chartType = 'boxplot';
      config = {
        xColumn: chart.xColumnName,
        yColumn: chart.yColumnName,
      };
    } else if (chart instanceof D3ViolinPlotChart) {
      chartType = 'violin';
      config = {
        xColumn: chart.xColumnName,
        yColumn: chart.yColumnName,
      };
    } else if (chart instanceof D3LineChart) {
      chartType = 'line';
      config = {
        xColumn: chart.xColumnName,
        yColumn: chart.yColumnName,
        colorBy: chart.colorBy || '',
        aggregation: chart.aggregationFunction,
      };
    } else if (chart instanceof D3DonutChart) {
      chartType = 'donut';
      config = {
        valueColumn: chart.valueColumnName,
        categoryColumn: chart.categoryColumnName,
        aggregation: chart.aggregationFunction,
      };
    } else if (chart instanceof D3StackedBarChart) {
      chartType = 'stackedbar';
      config = {
        xColumn: chart.xColumnName,
        yColumn: chart.yColumnName,
        stackColumn: chart.stackColumnName,
        aggregation: chart.aggregationFunction,
      };
    } else if (chart instanceof D3AreaChart) {
      chartType = 'area';
      config = {
        xColumn: chart.xColumnName,
        yColumn: chart.yColumnName,
        stackColumn: chart.stackColumnName,
        aggregation: chart.aggregationFunction,
      };
    }

    if (chartType) {
      (window as any).populateChartCreator?.(chartType, config);
    }
  }

  static findChartByDom(container: HTMLElement): any {
    return this.charts.find((comp) => comp.dom === container);
  }

  static redrawChart(container: HTMLElement): void {
    const chartComponent = this.findChartByDom(container);
    if (chartComponent) {
      if (chartComponent.chart) {
        chartComponent.chart.redrawWithoutRefetch();
      } else if (typeof chartComponent.redraw === 'function') {
        chartComponent.redraw();
      }
    }
  }
}

export class ResizeManager {
  private static instance: ResizeManager;
  private isResizing: boolean = false;
  private currentContainer: HTMLElement | null = null;
  private startX: number = 0;
  private startY: number = 0;
  private startWidth: number = 0;
  private startHeight: number = 0;

  private constructor() {
    this.onMouseMove = this.onMouseMove.bind(this);
    this.onMouseUp = this.onMouseUp.bind(this);
  }

  static getInstance(): ResizeManager {
    if (!ResizeManager.instance) {
      ResizeManager.instance = new ResizeManager();
    }
    return ResizeManager.instance;
  }

  onMouseDown(e: MouseEvent, chartContainer: HTMLElement, container: HTMLElement) {
    e.preventDefault();
    this.isResizing = true;
    this.currentContainer = chartContainer;
    const rect = chartContainer.getBoundingClientRect();
    this.startX = e.clientX;
    this.startY = e.clientY;
    this.startWidth = rect.width;
    this.startHeight = rect.height;

    window.addEventListener('mousemove', this.onMouseMove);
    window.addEventListener('mouseup', this.onMouseUp);
    container.style.cursor = 'nwse-resize';
    container.style.userSelect = 'none';
  }

  private onMouseMove(e: MouseEvent): void {
    if (!this.isResizing || !this.currentContainer) return;

    e.preventDefault();
    const minWidth = 500;
    const minHeight = 100;

    const deltaX = e.clientX - this.startX;
    const deltaY = e.clientY - this.startY;
    let newWidth = Math.max(minWidth, this.startWidth + deltaX);
    const newHeight = Math.max(minHeight, this.startHeight + deltaY);

    const parent = this.currentContainer.parentElement;
    if (parent) {
      const parentRect = parent.getBoundingClientRect();
      const containerRect = this.currentContainer.getBoundingClientRect();
      const maxAllowedWidth = parentRect.right - containerRect.left - 20;
      newWidth = Math.min(newWidth, maxAllowedWidth);
    }

    const numColumns = Math.ceil(newWidth / minWidth);
    const gridColumn = `span ${Math.max(1, numColumns)}`;
    if (this.currentContainer.style.gridColumn !== gridColumn) {
      this.currentContainer.style.gridColumn = gridColumn;
    }

    this.currentContainer.style.width = newWidth + 'px';
    this.currentContainer.style.height = newHeight + 'px';
    ChartManager.redrawChart(this.currentContainer);
    m.redraw();
  }

  private onMouseUp(): void {
    if (this.isResizing && this.currentContainer) {
      this.currentContainer.style.cursor = '';
      this.currentContainer.style.userSelect = '';
      this.isResizing = false;
      this.currentContainer = null;
      window.removeEventListener('mousemove', this.onMouseMove);
      window.removeEventListener('mouseup', this.onMouseUp);
    }
  }
}

class TooltipManager {
  private static instance: TooltipManager;
  private tooltip: any;

  private constructor() {
    this.tooltip = this.createTooltipElement();
  }

  static getInstance(): TooltipManager {
    if (!TooltipManager.instance) {
      TooltipManager.instance = new TooltipManager();
    }
    return TooltipManager.instance;
  }

  private createTooltipElement(): any {
    let tooltip = d3.select("body").select<HTMLDivElement>(".d3-chart-tooltip");
    if (tooltip.empty()) {
      tooltip = d3.select("body")
        .append("div")
        .attr("class", "d3-chart-tooltip");
    }
    return tooltip;
  }

  show(content: string, event: MouseEvent): void {
    this.tooltip
      .style("left", event.pageX + 10 + "px")
      .style("top", event.pageY - 10 + "px")
      .html(content)
      .style("visibility", "visible");
  }

  hide(): void {
    this.tooltip.style("visibility", "hidden");
  }

  addTooltip(selection: any, contentFn: (d: any, el: any) => string): void {
    selection
      .on("mouseover", (event: any, d: any) => {
        const content = contentFn(d, event.currentTarget);
        this.show(content, event);
      })
      .on("mousemove", (event: any) => {
        this.tooltip
          .style("left", event.pageX + 10 + "px")
          .style("top", event.pageY - 10 + "px");
      })
      .on("mouseout", () => {
        this.hide();
      });
  }
}

/**
 * Utilities for converting between internal filter format and DataGrid FilterDefinition format
 */
class FilterConverter {
  /**
   * Converts internal filter format to DataGrid FilterDefinition array
   */
  static toFilterDefinitions(filterMap: Record<string, any>): DataGridFilter[] {
    const filterDefs: DataGridFilter[] = [];

    for (const [column, filter] of Object.entries(filterMap)) {
      if (!filter || !filter.type) continue;

      filterDefs.push(...this.convertSingleFilter(column, filter));
    }

    return filterDefs;
  }

  private static convertSingleFilter(column: string, filter: any): DataGridFilter[] {
    switch (filter.type) {
      case 'IN':
        return this.convertInFilter(column, filter);
      case 'RANGE':
        return this.convertRangeFilter(column, filter);
      case 'LIKE':
        return this.convertLikeFilter(column, filter);
      case 'AND':
        return this.convertAndFilter(column, filter);
      default:
        return [];
    }
  }

  private static convertInFilter(column: string, filter: any): DataGridFilter[] {
    // Convert each value in the set to a separate '=' filter
    return Array.from(filter.values).map(value => ({
      column,
      op: '=' as const,
      value: value as any
    }));
  }

  private static convertRangeFilter(column: string, filter: any): DataGridFilter[] {
    return [{
      column,
      op: filter.operator as any,
      value: filter.value
    }];
  }

  private static convertLikeFilter(column: string, filter: any): DataGridFilter[] {
    return [{
      column,
      op: 'glob' as const,
      value: `*${filter.value}*`
    }];
  }

  private static convertAndFilter(column: string, filter: any): DataGridFilter[] {
    return filter.conditions.map((condition: any) => ({
      column,
      op: condition.operator as any,
      value: condition.value
    }));
  }

  /**
   * Converts DataGrid FilterDefinition array to internal filter format
   */
  static fromFilterDefinitions(
    filters: ReadonlyArray<DataGridFilter>,
    existingFilters: Record<string, any> = {}
  ): { filterMap: Record<string, any>, clearedColumns: Set<string> } {
    const filterMap: Record<string, any> = {};
    const clearedColumns = new Set<string>();

    // Detect columns where AND filters were partially removed
    const modifiedAndColumns = this.detectModifiedAndFilters(filters, existingFilters);
    for (const col of modifiedAndColumns) {
      clearedColumns.add(col);
    }

    // Group filters by column
    const filtersByColumn = d3.group(filters, (f: any) => f.column);

    for (const [column, columnFilters] of filtersByColumn) {
      // Skip columns where AND filter was modified (will be cleared)
      if (modifiedAndColumns.has(column)) continue;

      filterMap[column] = this.convertColumnFilters(column, columnFilters);
    }

    return { filterMap, clearedColumns };
  }

  private static detectModifiedAndFilters(
    currentFilters: ReadonlyArray<DataGridFilter>,
    existingFilters: Record<string, any>
  ): Set<string> {
    const modified = new Set<string>();

    for (const [column, existingFilter] of Object.entries(existingFilters)) {
      if (existingFilter?.type === 'AND') {
        const originalCount = existingFilter.conditions.length;
        const currentCount = currentFilters.filter(f => f.column === column).length;

        if (currentCount < originalCount) {
          modified.add(column);
        }
      }
    }

    return modified;
  }

  private static convertColumnFilters(
    column: string,
    columnFilters: DataGridFilter[]
  ): any {
    if (columnFilters.length === 0) return null;

    // Multiple filters for same column -> convert to IN filter
    if (columnFilters.length > 1) {
      return this.convertToInFilter(column, columnFilters);
    }

    // Single filter
    return this.convertSingleColumnFilter(column, columnFilters[0]);
  }

  private static convertToInFilter(column: string, filters: DataGridFilter[]): any {
    const values = new Set(
      filters
        .filter(f => 'value' in f && f.op === '=')
        .map(f => ('value' in f ? f.value : ''))
    );

    if (values.size === 0) return null;

    return {
      type: 'IN',
      values,
      raw: `${column} IN (${Array.from(values).join(', ')})`
    };
  }

  private static convertSingleColumnFilter(_column: string, filter: DataGridFilter): any {
    const value = 'value' in filter ? filter.value : undefined;
    const raw = 'value' in filter ? `${filter.op} ${value}` : filter.op;

    // Special case: '=' with non-numeric value -> treat as IN filter
    if (filter.op === '=' && !this.isNumeric(value)) {
      return {
        type: 'IN',
        values: new Set([value]),
        raw
      };
    }

    // Range operators
    if (['=', '!=', '<', '<=', '>', '>='].includes(filter.op)) {
      return {
        type: 'RANGE',
        operator: filter.op,
        value,
        raw
      };
    }

    // LIKE/glob operator
    if (filter.op === 'glob') {
      return {
        type: 'LIKE',
        value,
        raw
      };
    }

    // Custom/unknown operator
    return {
      type: 'CUSTOM',
      operator: filter.op,
      value,
      raw
    };
  }

  private static isNumeric(value: any): boolean {
    return typeof value === 'number' || typeof value === 'bigint';
  }
}

// Separate history tracking from active state
interface FilterEntry {
  filter: any;
  sourceChart: any;
  timestamp: number;
}

export class FilterManager {
  // Active filters (one per column - what's currently applied)
  private activeFilters: Map<string, FilterEntry> = new Map();

  // History stack (for undo) - per column
  private filterHistory: Map<string, FilterEntry[]> = new Map();

  // Subscribers
  private subscribers: Set<any> = new Set();

  private dataProvider: any;

  subscribe(component: any) {
    this.subscribers.add(component);
    const chartId = component?.dom?.id || 'unknown';
    console.log(`[FilterManager] Subscribed chart: ${chartId}`);
  }

  unsubscribe(component: any) {
    this.subscribers.delete(component);
    const chartId = component?.dom?.id || 'unknown';
    console.log(`[FilterManager] Unsubscribed chart: ${chartId}`);
  }

  setDataProvider(dataProvider: any) {
    this.dataProvider = dataProvider;
  }

  /**
   * Set filters from a chart. This:
   * 1. Pushes current filter to history (if exists)
   * 2. Sets new filter as active
   * 3. Notifies subscribers
   */
  async setFilters(
    filterMap: Record<string, any>,
    sourceChart: any,
  ): Promise<void> {
    const sourceChartId = sourceChart?.dom?.id || 'unknown';
    console.log(`[FilterManager] setFilters called from chart: ${sourceChartId}`);
    console.log(`[FilterManager] filterMap:`, filterMap);

    for (const [column, filter] of Object.entries(filterMap)) {
      // Save current filter to history before replacing
      const current = this.activeFilters.get(column);
      if (current) {
        const history = this.filterHistory.get(column) || [];
        history.push(current);
        this.filterHistory.set(column, history);
      }

      // Set new active filter
      this.activeFilters.set(column, {
        filter,
        sourceChart,
        timestamp: Date.now(),
      });
    }

    await this.notifySubscribers(sourceChart);
  }

  /**
   * Clear filters from a specific chart. This:
   * 1. Removes active filters from that chart
   * 2. Pops from history to restore previous filter
   * 3. Notifies subscribers
   */
  async clearFiltersForChart(sourceChart: any): Promise<void> {
    const sourceChartId = sourceChart?.dom?.id || 'unknown';
    console.log(`[FilterManager] clearFiltersForChart called for chart: ${sourceChartId}`);
    let changed = false;

    for (const [column, entry] of this.activeFilters.entries()) {
      if (entry.sourceChart === sourceChart) {
        // Remove current filter
        this.activeFilters.delete(column);
        changed = true;
        console.log(`[FilterManager] Removed filter for column: ${column}`);

        // Pop from history to restore previous
        const history = this.filterHistory.get(column);
        if (history && history.length > 0) {
          const previous = history.pop()!;
          this.activeFilters.set(column, previous);
          console.log(`[FilterManager] Restored previous filter for column: ${column}`);
        }
      }
    }

    if (changed) {
      if (typeof sourceChart.setIsFilterSource === 'function') {
        sourceChart.setIsFilterSource(false);
      }
      await this.notifySubscribers();
    }
  }

  /**
   * Get currently active filters (for applying to queries)
   */
  getFilters(): Record<string, any> {
    const result: Record<string, any> = {};
    for (const [column, entry] of this.activeFilters.entries()) {
      result[column] = entry.filter;
    }
    return result;
  }

  /**
   * Get filters formatted for data provider queries
   */
  getFiltersForQuery(): any[] {
    const filters: any[] = [];
    for (const [column, entry] of this.activeFilters.entries()) {
      filters.push(...this.expandFilter(column, entry.filter));
    }
    return filters;
  }

  /**
   * Expand compound filters (AND) into individual filter objects
   */
  private expandFilter(column: string, filter: any): any[] {
    if (filter.type === 'AND' && Array.isArray(filter.conditions)) {
      return filter.conditions.map((condition: any) => ({
        column,
        type: condition.type,
        operator: condition.operator,
        value: condition.value,
      }));
    }

    return [{
      column,
      type: filter.type,
      operator: filter.operator,
      value: filter.values ? Array.from(filter.values) : filter.value,
    }];
  }

  async notifySubscribers(sourceChart?: any) {
    console.log('[FilterManager] Notifying subscribers...');
    this.updateAllChartsFilterSourceState();
    this.updateURLWithFilters();

    const promises: Promise<void>[] = [];
    const globalUpdateSourceFlag = (window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER;
    console.log(`[FilterManager] Global update source flag: ${globalUpdateSourceFlag}`);

    this.subscribers.forEach((chart) => {
      const chartId = chart?.dom?.id || 'unknown';
      const isSourceChart = chart === sourceChart;
      if (!isSourceChart || globalUpdateSourceFlag) {
        console.log(`[FilterManager] Refreshing chart: ${chartId} (isSource: ${isSourceChart})`);
        promises.push(chart.refreshFilteredData(this.dataProvider));
      } else {
        console.log(`[FilterManager] Skipping source chart refresh: ${chartId}`);
      }
    });

    await Promise.all(promises);
    console.log('[FilterManager] All charts refreshed.');
  }

  // TODO(lalitm): implement this method.
  updateURLWithFilters() {}

  updateAllChartsFilterSourceState(): void {
    console.log('[FilterManager] Updating all charts filter source state...');
    const sourceCharts = new Set();
    for (const entry of this.activeFilters.values()) {
      if (entry.sourceChart) {
        sourceCharts.add(entry.sourceChart);
      }
    }

    this.subscribers.forEach((chart) => {
      const chartId = chart?.dom?.id || 'unknown';
      if (typeof chart.setIsFilterSource === 'function') {
        const isSource = sourceCharts.has(chart);
        console.log(`[FilterManager] Setting chart ${chartId} as filter source: ${isSource}`);
        chart.setIsFilterSource(isSource);
      }
    });
  }

  // TODO(lalitm): implement this method.
  clearAllFilters() {}
}

// --- From data_provider.ts ---
class DataProvider {
  type: string;
  config: any;
  constructor(type = 'memory', config = {}) {
    this.type = type;
    this.config = config;
  }
  async query(_querySpec: any): Promise<any> {
    throw new Error('DataProvider.query() must be implemented by subclass');
  }
}

export class MemoryDataProvider extends DataProvider {
  data: any[];

  constructor(data: any[]) {
    super('memory');
    this.data = data;
  }

  async query(querySpec: any): Promise<any> {
    let result = [...this.data];

    // Step 1: Apply filters
    if (querySpec.filters && querySpec.filters.length > 0) {
      result = this.applyFilters(result, querySpec.filters);
    }

    // Step 2: Apply aggregation
    if (querySpec.aggregation) {
      result = this.applyAggregation(result, querySpec.aggregation);
    }

    return { data: result };
  }

  // ============================================================================
  // FILTERING
  // ============================================================================

  private applyFilters(data: any[], filters: any[]): any[] {
    return data.filter(row => {
      return filters.every(filter => this.rowMatchesFilter(row, filter));
    });
  }

  private rowMatchesFilter(row: any, filter: any): boolean {
    const cellValue = row[filter.column];

    switch (filter.type) {
      case 'IN':
        const values = new Set(filter.value);
        return values.has(String(cellValue));

      case 'LIKE':
        return String(cellValue).toLowerCase().includes(String(filter.value).toLowerCase());

      case 'RANGE':
        return this.matchesRange(cellValue, filter);

      default:
        return true;
    }
  }

  private matchesRange(cellValue: any, filter: any): boolean {
    const numValue = Number(cellValue);
    if (isNaN(numValue)) return false;

    switch (filter.operator) {
      case '>': return numValue > filter.value;
      case '>=': return numValue >= filter.value;
      case '<': return numValue < filter.value;
      case '<=': return numValue <= filter.value;
      case '=': return numValue === filter.value;
      default: return true;
    }
  }

  // ============================================================================
  // AGGREGATION
  // ============================================================================

  private applyAggregation(data: any[], aggregation: any): any[] {
    const groupBy = Array.isArray(aggregation.groupBy)
      ? aggregation.groupBy
      : [aggregation.groupBy];

    // Group data by specified fields
    const grouped = d3.group(data, ...groupBy.map((field: string) => (d: any) => d[field]));

    // Recursively process groups and calculate aggregations
    const aggregatedData: any[] = [];
    this.processGroup(grouped, [], groupBy, aggregation, aggregatedData);

    return aggregatedData;
  }

  private processGroup(
    currentGroup: any,
    groupKeys: any[],
    groupByFields: string[],
    aggregation: any,
    result: any[]
  ): void {
    if (currentGroup instanceof Map) {
      // Still have nested groups - recurse deeper
      for (const [key, value] of currentGroup) {
        this.processGroup(value, [...groupKeys, key], groupByFields, aggregation, result);
      }
    } else {
      // Reached leaf level - this is our group data, aggregate it
      const aggregatedPoint = this.aggregateGroup(currentGroup, groupKeys, groupByFields, aggregation);
      result.push(aggregatedPoint);
    }
  }

  private aggregateGroup(
    groupData: any[],
    groupKeys: any[],
    groupByFields: string[],
    aggregation: any
  ): any {
    // Extract numeric values to aggregate
    const numericValues = groupData
      .map((d: any) => +d[aggregation.field])
      .filter((v: any) => !isNaN(v));

    // Calculate the aggregated value
    const aggregatedValue = this.calculateAggregate(numericValues, aggregation.function);

    // Build the result object
    const result: any = {
      __aggregated_value: aggregatedValue,
      __original_count: groupData.length,
    };

    // Add group-by fields to result
    groupByFields.forEach((field: string, i: number) => {
      result[field] = groupKeys[i];
    });

    return result;
  }

  private calculateAggregate(values: number[], func: string): number {
    if (values.length === 0) return 0;

    switch (func) {
      case 'sum':
        return d3.sum(values);
      case 'mean':
      case 'avg':
        return d3.mean(values) || 0;
      case 'count':
        return values.length;
      case 'min':
        return d3.min(values) || 0;
      case 'max':
        return d3.max(values) || 0;
      default:
        return d3.sum(values);
    }
  }

  // ============================================================================
  // STATISTICS (for specialized charts)
  // ============================================================================

  async stats(statsType: string, config: any): Promise<any> {
    let data = this.data;

    // Apply filters first
    if (config.filters && config.filters.length > 0) {
      data = this.applyFilters(data, config.filters);
    }

    switch (statsType) {
      case 'cdf':
        return this.computeCDF(data, config);
      case 'histogram':
        return this.computeHistogram(data, config);
      case 'boxplot':
        return this.computeBoxplot(data, config);
      case 'violin':
        return this.computeViolin(data, config);
      default:
        throw new Error(`Unknown stats type: ${statsType}`);
    }
  }

  // ----------------------------------------------------------------------------
  // CDF
  // ----------------------------------------------------------------------------

  private computeCDF(data: any[], config: any): any {
    if (!config.groupByColumn) {
      // Single CDF
      const values = data
        .map(d => +d[config.column])
        .filter(v => !isNaN(v) && isFinite(v));

      if (values.length === 0) {
        return { data: [] };
      }

      const sorted = [...values].sort(d3.ascending);
      const n = sorted.length;
      const cdf = sorted.map((value, i) => ({
        value: value,
        probability: (i + 1) / n,
        group: 'default'
      }));

      return { data: cdf };
    } else {
      // Grouped CDF
      const grouped = d3.group(data, (d: any) => d[config.groupByColumn]);
      const results: any[] = [];

      for (const [group, values] of grouped.entries()) {
        const numericValues = values
          .map((d: any) => +d[config.column])
          .filter((v: number) => !isNaN(v) && isFinite(v));

        if (numericValues.length === 0) continue;

        const sorted = [...numericValues].sort(d3.ascending);
        const n = sorted.length;
        const groupCdf = sorted.map((value, i) => ({
          value: value,
          probability: (i + 1) / n,
          group: group
        }));

        results.push(...groupCdf);
      }

      return { data: results };
    }
  }

  private computeHistogram(data: any[], config: any): any {
    const values = data
      .map(d => +d[config.column])
      .filter(v => !isNaN(v) && isFinite(v));

    if (values.length === 0) {
      return { data: [] };
    }

    const numBins = config.numBins || 30;
    const histogram = d3.histogram()
      .domain(d3.extent(values) as [number, number])
      .thresholds(numBins);

    const bins = histogram(values).map((bin: any, index: number) => ({
      x0: bin.x0,
      x1: bin.x1,
      count: bin.length,
      values: [...bin],
      index
    }));

    return { data: bins };
  }

  // ----------------------------------------------------------------------------
  // Boxplot
  // ----------------------------------------------------------------------------

  private computeBoxplot(data: any[], config: any): any {
    const grouped = d3.group(data, (d: any) => d[config.groupByColumn]);
    const results: any[] = [];

    for (const [key, values] of grouped.entries()) {
      const numericValues = values
        .map((d: any) => +d[config.valueColumn])
        .filter((v: number) => !isNaN(v) && isFinite(v));

      if (numericValues.length === 0) continue;

      const sorted = [...numericValues].sort(d3.ascending);
      const q1 = d3.quantile(sorted, 0.25) || 0;
      const median = d3.quantile(sorted, 0.5) || 0;
      const q3 = d3.quantile(sorted, 0.75) || 0;
      const iqr = q3 - q1;
      const min = Math.max(d3.min(sorted) || 0, q1 - 1.5 * iqr);
      const max = Math.min(d3.max(sorted) || 0, q3 + 1.5 * iqr);
      const outliers = sorted.filter((v: number) => v < min || v > max);

      // Count items in each section
      const counts = {
        lowerWhisker: sorted.filter(v => v >= min && v < q1).length,
        q1_median: sorted.filter(v => v >= q1 && v <= median).length,
        median_q3: sorted.filter(v => v > median && v <= q3).length,
        upperWhisker: sorted.filter(v => v > q3 && v <= max).length,
      };

      results.push({
        key,
        min,
        q1,
        median,
        q3,
        max,
        outliers,
        count: numericValues.length,
        counts
      });
    }

    return { data: results };
  }

  // ----------------------------------------------------------------------------
  // Violin Plot
  // ----------------------------------------------------------------------------

  private computeViolin(data: any[], config: any): any {
    const grouped = d3.group(data, (d: any) => d[config.groupByColumn]);
    const results: any[] = [];

    for (const [key, values] of grouped.entries()) {
      const numericValues = values
        .map((d: any) => +d[config.valueColumn])
        .filter((v: number) => !isNaN(v) && isFinite(v));

      if (numericValues.length === 0) continue;

      const sorted = [...numericValues].sort(d3.ascending);

      // Basic statistics
      const q1 = d3.quantile(sorted, 0.25) || 0;
      const median = d3.quantile(sorted, 0.5) || 0;
      const q3 = d3.quantile(sorted, 0.75) || 0;
      const min = d3.min(sorted) || 0;
      const max = d3.max(sorted) || 0;
      const p90 = d3.quantile(sorted, 0.90) || 0;
      const p95 = d3.quantile(sorted, 0.95) || 0;
      const p99 = d3.quantile(sorted, 0.99) || 0;

      // Density estimation for violin shape
      const bandwidth = 7;
      const ticks = d3.range(min, max, (max - min) / 40);
      const density = this.kernelDensityEstimation(sorted, ticks, bandwidth);

      results.push({
        key,
        min,
        q1,
        median,
        q3,
        max,
        p90,
        p95,
        p99,
        density,
        count: numericValues.length
      });
    }

    return { data: results };
  }

  private kernelDensityEstimation(
    values: number[],
    ticks: number[],
    bandwidth: number
  ): [number, number][] {
    return ticks.map(tick => {
      const density = d3.mean(values, v => {
        const u = (tick - v) / bandwidth;
        // Epanechnikov kernel
        return Math.abs(u) <= 1 ? 0.75 * (1 - u * u) / bandwidth : 0;
      });
      return [tick, density || 0];
    });
  }
}

export class ChartComponent implements m.Component<any> {
  chart: BaseChart|null = null;
  dom: HTMLElement|null = null;
  chartClass: any;
  chartTitle: string = '';

  constructor(chartClass: any) {
    this.chartClass = chartClass;
  }

  oncreate(vnode: m.VnodeDOM<any>) {
    this.dom = vnode.dom as HTMLElement;
    this.dom.id = `d3-chart-${Math.random().toString(36).substr(2, 9)}`;

    this.chart = new this.chartClass(this.dom, vnode.attrs, this);

    if (vnode.attrs.filterManager) {
      vnode.attrs.filterManager.subscribe(this);
    }

    if (this.chart) {
      this.chart.render().then(() => {
        this.updateChartTitle();
        m.redraw();
      });
    }
    ChartManager.register(this);
  }

  async refreshFilteredData(dataProvider?: any) {
    const chartId = this.dom?.id || 'unknown';
    console.log(`[ChartComponent] ${chartId} refreshFilteredData called`);
    if (this.chart) {
      if (dataProvider) {
        this.chart.dataProvider = dataProvider;
      }
      await this.chart.render();
      this.updateChartTitle();
      m.redraw();
    }
  }

  updateChartTitle() {
    if (!this.chart) return;

    const chart: any = this.chart;
    let columns: string[] = [];

    if (chart.xColumnName) columns.push(chart.xColumnName);
    if (chart.yColumnName) columns.push(chart.yColumnName);
    if (chart.valueColumnName) columns.push(chart.valueColumnName);
    if (chart.categoryColumnName) columns.push(chart.categoryColumnName);
    if (chart.stackColumnName) columns.push(chart.stackColumnName);
    if (chart.columnName) columns.push(chart.columnName);
    if (chart.colorBy) columns.push(chart.colorBy);

    // Remove duplicates
    columns = [...new Set(columns)];

    if (columns.length > 0) {
      this.chartTitle = columns.join(' x ');
    }
  }

  setIsFilterSource(isSource: boolean) {
    const chartId = this.dom?.id || 'unknown';
    console.log(`[ChartComponent] ${chartId} setIsFilterSource called with: ${isSource}`);
    if (this.chart) {
      this.chart.isFilterSource = isSource;
      if (this.dom) {
        const header = this.dom.querySelector('.chart-header');
        if (header) {
          header.classList.toggle('filter-source', isSource);
        }
      }
      if (isSource && (window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER === true) {
        this.refreshFilteredData();
      }
    }
  }

  onremove(vnode: any) {
    if (vnode.attrs.filterManager) {
      vnode.attrs.filterManager.unsubscribe(this);
      vnode.attrs.filterManager.clearFiltersForChart(this);
    }

    if (this.chart) {
      this.chart.destroy();
    }
    ChartManager.unregister(this);
  }

  view() {
    const isFilterSource = this.chart ? this.chart.isFilterSource : false;
    return m(
        '.chart-container',
        {style: {height: '400px'}},
        m(`.chart-header${isFilterSource ? '.filter-source' : ''}`,
          {
            onclick: (event: MouseEvent) => {
              event.stopPropagation();
              if (this.chart?.filterManager) {
                this.chart.filterManager.clearFiltersForChart(this);
              }
            },
          },
          m('h4.chart-title', this.chart ? this.chartTitle : ''),
          m('.chart-actions',
            m('button.chart-action-btn',
              {
                title: 'Duplicate chart',
                onclick: () => ChartManager.duplicateChart(this.dom)
              },
              '⧉'),
            m('button.chart-close-btn',
              {
                title: 'Remove Chart',
                onclick: () => ChartManager.removeChart(this.dom)
              },
              '×'),
            ),
          ),
        m('.chart-content',
          {style: {'overflow-x': 'auto', 'overflow-y': 'hidden'}}),
        m('.resize-handle', {
          onmousedown: (e: MouseEvent) => {
            if (this.dom) {
              ResizeManager.getInstance().onMouseDown(e, this.dom, this.dom);
            }
          },
        }),
    );
  }
}

class FilterBuilder {
  static createInFilter(column: string, values: any[]): any {
    return {
      [column]: {
        type: 'IN',
        values: new Set(values),
        raw: `${column} IN (${values.join(', ')})`
      }
    };
  }

  static createRangeFilter(column: string, min: number, max: number): any {
    return {
      [column]: {
        type: 'AND',
        conditions: [
          { type: 'RANGE', operator: '>=', value: min },
          { type: 'RANGE', operator: '<=', value: max }
        ],
        raw: `${column}: ${min.toFixed(2)} - ${max.toFixed(2)}`
      }
    };
  }

  static createMultiColumnRange(columns: string[], ranges: [number, number][]): any {
    const filters: any = {};
    columns.forEach((col, i) => {
      const [min, max] = ranges[i];
      filters[col] = this.createRangeFilter(col, min, max)[col];
    });
    return filters;
  }
}

class LoggingHelper {
  constructor(private chart: BaseChart) {}

  log(message: string, data?: any) {
    const chartId = this.chart.container.id || 'unknown';
    if (data !== undefined) {
      console.log(`[${chartId}] ${message}`, data);
    } else {
      console.log(`[${chartId}] ${message}`);
    }
  }
}

class ScaleHelper {
  createLinearScale(
    data: any[],
    accessor: (d: any) => number,
    range: [number, number],
    startFromZero: boolean = false
  ) {
    const values = data.map(accessor).filter(v => !isNaN(v) && isFinite(v));
    const extent = d3.extent(values) as [number, number];
    const domain = startFromZero ? [0, extent[1]] : extent;

    return d3.scaleLinear()
      .domain(domain)
      .range(range)
      .nice();
  }

  createBandScale(
    data: any[],
    accessor: (d: any) => string,
    range: [number, number],
    padding: number = 0.1
  ) {
    return d3.scaleBand()
      .domain(data.map(accessor))
      .range(range)
      .padding(padding);
  }

  createColorScale(values: any[], scheme: any = d3.schemeCategory10) {
    return d3.scaleOrdinal(scheme).domain(values);
  }
}

class AxisHelper {
  constructor(private chart: BaseChart) {}

  createXAxis(scale: any, rotate: boolean = true): any {
    const axis = d3.axisBottom(scale);
    const g = this.chart.g.append('g')
      .attr('transform', `translate(0,${this.chart.height - this.chart.margin.top - this.chart.margin.bottom})`)
      .call(axis);

    if (rotate) {
      g.selectAll('text')
        .style('text-anchor', 'end')
        .attr('dx', '-.8em')
        .attr('dy', '.15em')
        .attr('transform', 'rotate(-45)')
        .text((d: any, i: number) => {
          if (i % 2 !== 0) return '';
          return truncate(String(d), 10);
        });
    }
    return g;
  }

  createYAxis(scale: any): any {
    return this.chart.g.append('g')
      .call(d3.axisLeft(scale).tickFormat((d: any) => truncate(String(d), 10)));
  }

  addGridLines(yScale: any) {
    const chartWidth = this.chart.width - this.chart.margin.left - this.chart.margin.right;
    this.chart.g.append('g')
      .attr('class', 'grid')
      .call(d3.axisLeft(yScale).tickSize(-chartWidth).tickFormat('' as any))
      .call((g: any) => g.select('.domain').remove())
      .selectAll('line')
      .attr('stroke', 'rgba(0,0,0,0.1)');
  }
}

class BrushHelper {
  constructor(protected chart: BaseChart) {}

  createBrushHandler(
    brushType: 'x' | 'xy',
    onSelection: (selection: any) => { filters: any, selectedItems: any[] }
  ) {
    const chartWidth = this.chart.width - this.chart.margin.left - this.chart.margin.right;
    const chartHeight = this.chart.height - this.chart.margin.top - this.chart.margin.bottom;

    const brush = brushType === 'x'
      ? d3.brushX().extent([[0, 0], [chartWidth, chartHeight]])
      : d3.brush().extent([[0, 0], [chartWidth, chartHeight]]);

    return brush.on('end', (event: { selection: any; }) => {
      this.chart.loggingHelper.log('Brush event ended.');

      if (!event.selection) {
        this.chart.loggingHelper.log('No selection, clearing filters.');
        this.chart.clearVisualSelection();
        if (this.chart.filterManager) {
          this.chart.filterManager.clearFiltersForChart(this.chart.mithrilComponent);
        }
        return;
      }

      const { filters, selectedItems } = onSelection(event.selection);
      this.chart.applyVisualSelection(selectedItems);

      if (this.chart.filterManager && Object.keys(filters).length > 0) {
        this.chart.loggingHelper.log('Applying filters', filters);
        this.chart.filterManager.setFilters(filters, this.chart.mithrilComponent);
      }
    });
  }
}

class ClippingBrushHelper extends BrushHelper {
  constructor(chart: BaseChart, private clipPathManager: ClipPathManager) {
    super(chart);
  }

  handleBrush(
    selection: any,
    mainGroupSelector: string,
    drawCallback: (container: any, data: any[], opacity: number) => void,
    filterCallback: (selection: any) => any
  ) {
    this.chart.loggingHelper.log('Brush event handled.');
    this.chart.g.selectAll('.dimmed').remove();
    this.chart.g.selectAll('.highlight').remove();
    this.clipPathManager.removeAllClips();

    if (!selection) {
      this.chart.loggingHelper.log('No selection, clearing filters.');
      this.chart.g.selectAll(mainGroupSelector).style('opacity', 1);
      if (this.chart.filterManager) {
        this.chart.filterManager.clearFiltersForChart(this.chart.mithrilComponent);
      }
      return;
    }

    this.chart.g.selectAll(mainGroupSelector).style('opacity', 0);
    drawCallback(this.chart.g.append('g').attr('class', 'dimmed'), this.chart.chartData, 0.2);

    let x0, y0, x1, y1;
    if (Array.isArray(selection[0])) {
      // 2D brush selection [[x0, y0], [x1, y1]]
      [[x0, y0], [x1, y1]] = selection;
    } else {
      // 1D brush selection [x0, x1]
      [x0, x1] = selection;
      y0 = 0;
      y1 = this.chart.height;
    }
    const clipUrl = this.clipPathManager.createRectClip(x0, y0, x1 - x0, y1 - y0);
    const highlightGroup = this.chart.g.append('g')
      .attr('class', 'highlight')
      .attr('clip-path', clipUrl);
    drawCallback(highlightGroup, this.chart.chartData, 1.0);

    if (this.chart.filterManager && selection) {
      this.chart.loggingHelper.log('Applying filter for brush selection');
      const filters = filterCallback(selection);
      this.chart.filterManager.setFilters(filters, this.chart.mithrilComponent);
    }
  }
}

class TooltipHelper {
  addTooltipToSelection(
    selection: any,
    contentBuilder: (d: any, el?: any) => Record<string, any>
  ) {
    const tooltipManager = TooltipManager.getInstance();
    tooltipManager.addTooltip(selection, (d: any, el: any) => {
      const content = contentBuilder(d, el);
      return Object.entries(content)
        .map(([key, value]) => `<strong>${key}:</strong> ${value}`)
        .join('<br>');
    });
  }
}

class LegendHelper {
  constructor(private chart: BaseChart) {}

  renderLegend(colorScale: any, onClickColumn: string) {
    const chartContent = this.chart.container.querySelector('.chart-content');
    if (!chartContent) return;
    d3.select(chartContent).select('.chart-legend').remove();

    const legendContainer = d3.select(chartContent)
      .append('div')
      .attr('class', 'chart-legend');

    legendContainer.on('click', (event: MouseEvent) => {
      if (event.target === legendContainer.node() && this.chart.filterManager) {
        this.chart.filterManager.clearFiltersForChart(this.chart.mithrilComponent);
      }
    });

    const maxLegendItems = 10;
    const legendData = colorScale.domain();
    const truncated = legendData.length > maxLegendItems;
    const data = truncated ? legendData.slice(0, maxLegendItems) : legendData;

    const legendItems = legendContainer.selectAll('.legend-item')
      .data(data)
      .enter()
      .append('div')
      .attr('class', 'legend-item')
      .on('click', (event: MouseEvent, d: any) => {
        event.stopPropagation();
        if (this.chart.filterManager) {
          this.chart.filterManager.setFilters(
            FilterBuilder.createInFilter(onClickColumn, [d]),
            this.chart.mithrilComponent
          );
        }
        if (!(window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER) {
          legendItems.style('opacity', (item: any) => (item === d ? 1.0 : 0.2));
        }
      });

    legendItems.append('span')
      .attr('class', 'legend-swatch')
      .style('background-color', (d: any) => colorScale(d));

    legendItems.append('span')
      .attr('class', 'legend-label')
      .text((d: any) => truncate(d, 20));

    if (truncated) {
      legendContainer.append('div')
        .attr('class', 'legend-item')
        .append('span')
        .attr('class', 'legend-label')
        .text('...');
    }
  }
}


abstract class BaseChart {
  container: HTMLElement;
  width: number = 600;
  height: number = 400;
  margin = {top: 10, right: 30, bottom: 150, left: 60};
  svg: any;
  g: any;
  yScale: any;
  chartData: any[] = [];
  dataProvider: any;
  filterManager: any;
  isFilterSource: boolean = false;
  mithrilComponent: any;

  // Helpers
  loggingHelper: LoggingHelper;
  scaleHelper: ScaleHelper;
  axisHelper: AxisHelper;
  brushHelper: BrushHelper;
  tooltipHelper: TooltipHelper;
  legendHelper: LegendHelper;

  constructor(container: HTMLElement, attrs: any, mithrilComponent: any) {
    this.container = container;
    this.dataProvider = attrs.dataProvider;
    this.filterManager = attrs.filterManager;
    this.mithrilComponent = mithrilComponent;

    this.loggingHelper = new LoggingHelper(this);
    this.scaleHelper = new ScaleHelper();
    this.axisHelper = new AxisHelper(this);
    this.brushHelper = new BrushHelper(this);
    this.tooltipHelper = new TooltipHelper();
    this.legendHelper = new LegendHelper(this);
  }

  // ============================================================================
  // LOGGING
  // ============================================================================

  protected log(message: string, data?: any) {
    this.loggingHelper.log(message, data);
  }

  // ============================================================================
  // DIMENSIONS & SETUP
  // ============================================================================

  getContainerDimensions() {
    const rect = this.container.getBoundingClientRect();
    return {
      width: rect.width || 600,
      height: rect.height || 400,
    };
  }

  setupSvgWithDimensions(chartContent: any) {
    if (!chartContent) return null;

    d3.select(chartContent).selectAll('*').remove();

    const containerDimensions = this.getContainerDimensions();
    this.width = containerDimensions.width;
    this.height = containerDimensions.height;

    this.svg = d3.select(chartContent)
                     .append('svg')
                     .attr('width', this.width)
                     .attr('height', this.height);

    this.g = this.svg.append('g').attr(
        'transform', `translate(${this.margin.left},${this.margin.top})`);

    return this.svg;
  }

  // ============================================================================
  // SCALE CREATION HELPERS
  // ============================================================================

  protected createLinearScale(
    data: any[],
    accessor: (d: any) => number,
    range: [number, number],
    startFromZero: boolean = false
  ) {
    return this.scaleHelper.createLinearScale(data, accessor, range, startFromZero);
  }

  protected createBandScale(
    data: any[],
    accessor: (d: any) => string,
    range: [number, number],
    padding: number = 0.1
  ) {
    return this.scaleHelper.createBandScale(data, accessor, range, padding);
  }

  protected createColorScale(values: any[], scheme: any = d3.schemeCategory10) {
    return this.scaleHelper.createColorScale(values, scheme);
  }

  // ============================================================================
  // AXIS CREATION HELPERS
  // ============================================================================

  protected createXAxis(scale: any, rotate: boolean = true): any {
    return this.axisHelper.createXAxis(scale, rotate);
  }

  protected createYAxis(scale: any): any {
    return this.axisHelper.createYAxis(scale);
  }

  protected addGridLines(yScale: any) {
    this.axisHelper.addGridLines(yScale);
  }

  // ============================================================================
  // BRUSH HANDLING
  // ============================================================================

  protected createBrushHandler(
    brushType: 'x' | 'xy',
    onSelection: (selection: any) => { filters: any, selectedItems: any[] }
  ) {
    return this.brushHelper.createBrushHandler(brushType, onSelection);
  }

  // ============================================================================
  // VISUAL SELECTION HELPERS
  // ============================================================================

  applyVisualSelection(selectedItems: any[], selector: string = '.selectable') {
    const selectedSet = new Set(selectedItems);
    const isEmpty = selectedItems.length === 0;

    this.g.selectAll(selector).style('opacity', (d: any) =>
      isEmpty || selectedSet.has(d) ? 1.0 : 0.2
    );
  }

  clearVisualSelection(selector: string = '.selectable') {
    this.g.selectAll(selector).style('opacity', 1.0);
  }

  // ============================================================================
  // TOOLTIP HELPERS
  // ============================================================================

  protected addTooltipToSelection(
    selection: any,
    contentBuilder: (d: any, el?: any) => Record<string, any>
  ) {
    this.tooltipHelper.addTooltipToSelection(selection, contentBuilder);
  }

  // ============================================================================
  // LEGEND HELPERS
  // ============================================================================

  protected renderLegend(colorScale: any, onClickColumn: string) {
    this.legendHelper.renderLegend(colorScale, onClickColumn);
  }

  // ============================================================================
  // DATA LOADING
  // ============================================================================

  async loadData() {
    this.log('Loading data...');
    this.log('FilterManager active filters:',
      this.filterManager ? this.filterManager.getFilters() : 'No FilterManager');

    const querySpec = this.buildQuerySpec();
    const querySpecWithFilters = {
      ...querySpec,
      filters: this.filterManager ? this.filterManager.getFiltersForQuery() : [],
    };

    const result = await this.dataProvider.query(querySpecWithFilters);
    this.chartData = this.processLoadedData(result.data);

    this.log('Data loaded:', this.chartData);
    this.log('Chart data length:', this.chartData.length);
  }

  abstract buildQuerySpec(): any;

  processLoadedData(data: any[]): any[] {
    return data;
  }

  // ============================================================================
  // RENDERING
  // ============================================================================

  async render() {
    this.log('Rendering chart...');
    await this.loadData();

    const chartContent = this.container.querySelector('.chart-content');
    if (!chartContent) {
      this.log('Chart content area not found.');
      return;
    }

    this.setupSvgWithDimensions(chartContent);

    if (!this.chartData || this.chartData.length === 0) {
      this.log('No data to render');
      this.renderEmptyState(this.getEmptyStateMessage());
      return;
    }

    await this.renderChart();
  }

  renderEmptyState(message: string) {
    this.g.append('text')
      .attr('x', (this.width - this.margin.left - this.margin.right) / 2)
      .attr('y', (this.height - this.margin.top - this.margin.bottom) / 2)
      .attr('text-anchor', 'middle')
      .style('font-size', '14px')
      .style('fill', '#666')
      .text(message);
  }

  abstract renderChart(): Promise<void>;

  protected getEmptyStateMessage(): string {
    return 'No data available';
  }

  async drawChart() {
    await this.render();
  }

  redrawWithoutRefetch() {
    this.render();
  }

  destroy() {}

  // ============================================================================
  // UTILITY
  // ============================================================================

  protected extractNumericValues(data: any[], accessor: (d: any) => any): number[] {
    return data
      .map(accessor)
      .filter(v => typeof v === 'number' && !isNaN(v) && isFinite(v));
  }
}

// --- Simple D3BarChart class ---
class D3BarChart extends BaseChart {
  xColumnName: string;
  yColumnName: string;
  aggregationFunction: string;

  constructor(container: HTMLElement, attrs: any, mithrilComponent: any) {
    super(container, attrs, mithrilComponent);
    this.xColumnName = attrs.xColumnName;
    this.yColumnName = attrs.yColumnName;
    this.aggregationFunction = attrs.aggregationFunction;
  }

  buildQuerySpec(): any {
    return {
      aggregation: {
        groupBy: this.xColumnName,
        field: this.yColumnName,
        function: this.aggregationFunction
      }
    };
  }

  async renderChart() {
    const chartWidth = this.width - this.margin.left - this.margin.right;
    const chartHeight = this.height - this.margin.top - this.margin.bottom;

    // Create scales using base class helpers
    const xScale = this.createBandScale(
      this.chartData,
      d => d[this.xColumnName],
      [0, chartWidth]
    );

    const yScale = this.createLinearScale(
      this.chartData,
      d => d.__aggregated_value,
      [chartHeight, 0],
      true // start from zero
    );

    // Create brush with simplified logic
    const brush = this.createBrushHandler('x', (selection) => {
      const [x0, x1] = selection;

      const selectedData = this.chartData.filter((d: any) => {
        const barX = xScale(d[this.xColumnName]);
        if (barX === undefined) return false;
        const barWidth = xScale.bandwidth();
        return barX + barWidth > x0 && barX < x1;
      });

      this.log(`Selected ${selectedData.length} bars out of ${this.chartData.length} total bars.`);

      const selectedValues = selectedData.map((d: any) => d[this.xColumnName]);

      return {
        filters: FilterBuilder.createInFilter(this.xColumnName, selectedValues),
        selectedItems: selectedData
      };
    });

    this.g.append('g')
      .attr('class', 'brush')
      .call(brush);

    // Draw bars
    const bars = this.g.selectAll('.bar')
      .data(this.chartData)
      .enter().append('rect')
      .attr('class', 'bar selectable') // Added 'selectable' class for visual selection
      .attr('x', (d: any) => xScale(d[this.xColumnName]))
      .attr('y', (d: any) => yScale(d.__aggregated_value))
      .attr('width', xScale.bandwidth())
      .attr('height', (d: any) => chartHeight - yScale(d.__aggregated_value))
      .attr('fill', 'steelblue');

    // Add axes and grid
    this.createXAxis(xScale);
    this.createYAxis(yScale);
    this.addGridLines(yScale);

    // Add tooltip using base class helper
    this.addTooltipToSelection(bars, (d: any) => ({
      [this.xColumnName]: d[this.xColumnName],
      [this.yColumnName]: d.__aggregated_value
    }));
  }
}

class D3HistogramChart extends BaseChart {
  columnName: string;
  numBins: number = 20;

  constructor(container: HTMLElement, attrs: any, mithrilComponent: any) {
    super(container, attrs, mithrilComponent);
    this.columnName = attrs.columnName;
  }

  buildQuerySpec(): any {
    return {}; // Histogram uses stats, not query
  }

  async loadData() {
    this.log('Loading histogram data...');
    this.log('FilterManager active filters:',
      this.filterManager ? this.filterManager.getFilters() : 'No FilterManager');

    const config = {
      filters: this.filterManager ? this.filterManager.getFiltersForQuery() : [],
      column: this.columnName,
      numBins: this.numBins
    };

    const result = await this.dataProvider.stats('histogram', config);
    this.chartData = this.processLoadedData(result.data);

    this.log(`Histogram data loaded: ${this.chartData.length} bins`);
  }

  async renderChart() {
    this.log('Starting histogram renderChart...');
    const chartWidth = this.width - this.margin.left - this.margin.right;
    const chartHeight = this.height - this.margin.top - this.margin.bottom;
    this.log(`Chart dimensions: ${chartWidth}x${chartHeight}`);

    const xExtent = d3.extent(this.chartData.flatMap((d: any) => [d.x0, d.x1]));
    this.log('X extent:', xExtent);
    const xScale = d3.scaleLinear()
      .domain(xExtent as [number, number])
      .range([0, chartWidth]);

    const yScale = this.createLinearScale(
      this.chartData,
      d => d.count,
      [chartHeight, 0],
      true
    );
    this.log('Y scale domain:', yScale.domain());

    const brush = this.createBrushHandler('x', (selection) => {
      const [x0, x1] = selection;
      const min = xScale.invert(x0);
      const max = xScale.invert(x1);

      this.log('Brush selection inverted to data range:', [min, max]);

      const selectedData = this.chartData.filter((d: any) =>
        d.x1 > min && d.x0 < max
      );

      return {
        filters: FilterBuilder.createRangeFilter(this.columnName, min, max),
        selectedItems: selectedData
      };
    });

    this.g.append('g')
      .attr('class', 'brush')
      .call(brush);

    this.log('Creating histogram bars...');
    const bars = this.g.selectAll('.bar')
      .data(this.chartData)
      .enter().append('rect')
      .attr('class', 'bar selectable')
      .attr('x', (d: any) => {
        const x = xScale(d.x0);
        this.log(`Bar x position for bin [${d.x0}, ${d.x1}]: ${x}`);
        return x;
      })
      .attr('y', (d: any) => {
        const y = yScale(d.count);
        this.log(`Bar y position for count ${d.count}: ${y}`);
        return y;
      })
      .attr('width', (d: any) => {
        const width = Math.max(0, xScale(d.x1) - xScale(d.x0) - 1);
        this.log(`Bar width for bin [${d.x0}, ${d.x1}]: ${width}`);
        return width;
      })
      .attr('height', (d: any) => {
        const height = chartHeight - yScale(d.count);
        this.log(`Bar height for count ${d.count}: ${height}`);
        return height;
      })
      .attr('fill', 'steelblue');
    
    this.log(`Created ${bars.size()} histogram bars`);

    this.addTooltipToSelection(bars, (d: any) => ({
      'Range': `[${d.x0.toFixed(2)}, ${d.x1.toFixed(2)})`,
      'Count': d.count
    }));

    this.log('Adding axes and grid lines...');
    this.createXAxis(xScale);
    this.createYAxis(yScale);
    this.addGridLines(yScale);
    this.log('Histogram renderChart completed');
  }
}

class ClipPathManager {
  private svg: any;
  private defs: any;
  private clipPathCounter: number = 0;
  private activeClips: Set<string> = new Set();

  constructor(svg: any) {
    this.svg = svg;
    this.defs = this.ensureDefs();
  }

  private ensureDefs(): any {
    let defs = this.svg.select("defs");
    if (defs.empty()) {
      defs = this.svg.append("defs");
    }
    return defs;
  }

  createRectClip(x: number, y: number, width: number, height: number): string {
    const id = `clip-${this.clipPathCounter++}`;
    this.activeClips.add(id);

    this.defs.append("clipPath")
      .attr("id", id)
      .append("rect")
      .attr("x", x)
      .attr("y", y)
      .attr("width", width)
      .attr("height", height);

    return `url(#${id})`;
  }

  removeAllClips(): void {
    this.activeClips.forEach((id) => {
      this.defs.select(`#${id}`).remove();
    });
    this.activeClips.clear();
  }
}

class D3CDFChart extends BaseChart {
  cdfData: any[] = [];
  columnName: string;
  colorBy: string | null;
  line: any;
  xScale: any;
  colorScale: any;

  constructor(container: HTMLElement, attrs: any, mithrilComponent: any) {
    super(container, attrs, mithrilComponent);
    this.columnName = attrs.columnName;
    this.colorBy = attrs.colorBy || null;
    if (this.colorBy) {
      this.margin.right = 180;
    }
  }

  buildQuerySpec(): any {
    return {}; // CDF uses stats, not query
  }

  async loadData() {
    this.log('Loading CDF data...');
    this.log('FilterManager active filters:',
      this.filterManager ? this.filterManager.getFilters() : 'No FilterManager');

    const config = {
      filters: this.filterManager ? this.filterManager.getFiltersForQuery() : [],
      column: this.columnName,
      groupByColumn: this.colorBy
    };

    const result = await this.dataProvider.stats('cdf', config);
    this.chartData = this.processLoadedData(result.data);

    this.log(`CDF data loaded: ${this.chartData.length} points`);
  }

  processLoadedData(data: any[]): any[] {
    if (!this.colorBy) {
      this.cdfData = [data];
      return data;
    } else {
      const grouped = d3.group(data, (d: any) => d.group);
      this.cdfData = Array.from(grouped.values());
      return data;
    }
  }

  async renderChart() {
    const chartWidth = this.width - this.margin.left - this.margin.right;
    const chartHeight = this.height - this.margin.top - this.margin.bottom;

    this.xScale = this.createLinearScale(
      this.chartData,
      d => d.value,
      [0, chartWidth]
    );

    this.yScale = d3.scaleLinear()
      .domain([0, 1])
      .range([chartHeight, 0]);

    this.line = d3.line()
      .x((d: any) => this.xScale(d.value))
      .y((d: any) => this.yScale(d.probability))
      .curve(d3.curveStepAfter);

    if (this.colorBy) {
      this.colorScale = this.createColorScale(
        this.cdfData.map((groupCdf: any) => groupCdf[0].group)
      );
      this.cdfData.forEach((groupCdf: any) => {
        this.g.append('path')
          .datum(groupCdf)
          .attr('class', 'cdf-line')
          .attr('fill', 'none')
          .attr('stroke', this.colorScale(groupCdf[0].group))
          .attr('stroke-width', 1.5)
          .attr('d', this.line);
      });
    } else {
      this.g.append('path')
        .datum(this.cdfData[0])
        .attr('class', 'cdf-line')
        .attr('fill', 'none')
        .attr('stroke', 'steelblue')
        .attr('stroke-width', 1.5)
        .attr('d', this.line);
    }

    this.addInteractiveOverlay();

    // Use manual brush handler instead of createBrushHandler because CDF has custom highlighting
    const brush = d3.brushX()
      .extent([[0, 0], [chartWidth, chartHeight]])
      .on('end', (event: { selection: any; }) => {
        this.log('Brush event ended.');

        if (!event.selection) {
          this.log('No selection, clearing filters.');
          this.highlightSelection([]); // Clear highlights
          if (this.filterManager) {
            this.filterManager.clearFiltersForChart(this.mithrilComponent);
          }
          return;
        }

        const selectedData = this.getBrushedItems(event.selection);
        this.highlightSelection(selectedData); // Apply custom CDF highlighting

        if (this.filterManager && selectedData.length > 0) {
          this.log(`Applying CDF filter with ${selectedData.length} selected points`);

          const values = selectedData.map(d => d.value);
          const valueRange = d3.extent(values) as [number, number];

          const filters = FilterBuilder.createRangeFilter(
            this.columnName,
            valueRange[0],
            valueRange[1]
          );

          this.filterManager.setFilters(filters, this.mithrilComponent);
        }
      });

    this.g.append('g')
      .attr('class', 'brush')
      .call(brush);

    this.createXAxis(this.xScale);
    this.g.append('g')
      .call(d3.axisLeft(this.yScale).tickFormat((d: any) =>
        truncate(String(d3.format('.0%')(d)), 10)
      ));
    this.addGridLines(this.yScale);

    if (this.colorBy) {
      this.renderLegend(this.colorScale, this.colorBy);
    }
  }

  addInteractiveOverlay() {
    const chartWidth = this.width - this.margin.left - this.margin.right;
    const chartHeight = this.height - this.margin.top - this.margin.bottom;
    const tooltipManager = TooltipManager.getInstance();

    const focus = this.g.append('g')
      .attr('class', 'focus')
      .style('display', 'none');

    focus.append('circle').attr('r', 5);

    this.g.append('rect')
      .attr('class', 'overlay')
      .attr('width', chartWidth)
      .attr('height', chartHeight)
      .style('fill', 'none')
      .style('pointer-events', 'all')
      .on('mouseover', () => {
        focus.style('display', null);
        tooltipManager.show('', {pageX: 0, pageY: 0} as MouseEvent);
      })
      .on('mouseout', () => {
        focus.style('display', 'none');
        tooltipManager.hide();
      })
      .on('mousemove', (event: any) => {
        const bisect = d3.bisector((d: any) => d.value).left;
        const [mouseX, mouseY] = d3.pointer(event);
        const x0 = this.xScale.invert(mouseX);

        let closestPoint: any = null;
        let minDistance = Infinity;

        for (const group of this.cdfData) {
          const i = bisect(group, x0, 1);
          const d0 = group[i - 1];
          const d1 = group[i];
          let d;
          if (d0 && d1) {
            d = (x0 - d0.value) > (d1.value - x0) ? d1 : d0;
          } else {
            d = d0 || d1;
          }

          if (d) {
            const dx = this.xScale(d.value) - mouseX;
            const dy = this.yScale(d.probability) - mouseY;
            const distance = Math.sqrt(dx * dx + dy * dy);
            if (distance < minDistance) {
              minDistance = distance;
              closestPoint = d;
            }
          }
        }

        if (closestPoint) {
          focus.attr('transform',
            `translate(${this.xScale(closestPoint.value)},${this.yScale(closestPoint.probability)})`
          );
          tooltipManager.show(`
            <strong>Value:</strong> ${closestPoint.value}<br>
            <strong>Probability:</strong> ${closestPoint.probability.toFixed(2)}
          `, event);
        }
      });
  }

  getBrushedItems(selection: any): any[] {
    const [x0, x1] = selection;
    const minValue = this.xScale.invert(x0);
    const maxValue = this.xScale.invert(x1);

    const flatData = this.cdfData.flat().sort((a: any, b: any) => a.value - b.value);
    const selectedData: any[] = [];

    for (let i = 0; i < flatData.length; i++) {
      const currentPoint = flatData[i];
      const nextPoint = flatData[i + 1];

      const currentX = currentPoint.value;
      const nextX = nextPoint ? nextPoint.value : currentX;

      const segmentStartsAfterBrush = currentX >= maxValue;
      const segmentEndsBeforeBrush = nextX <= minValue;

      if (!segmentStartsAfterBrush && !segmentEndsBeforeBrush) {
        selectedData.push(currentPoint);
      }
    }
    return selectedData;
  }

  highlightSelection(selectedData: any[]): void {
    this.g.selectAll('.cdf-line-highlight').remove();

    const totalPoints = this.cdfData.reduce((sum: any, group: any) => sum + group.length, 0);
    const hasSelection = selectedData.length > 0 && selectedData.length < totalPoints;

    this.g.selectAll('.cdf-line').style('opacity', hasSelection ? 0.2 : 1.0);

    if (hasSelection) {
      const selectedByGroup = d3.group(selectedData, (d: any) => d.group);
      for (const [group, groupSelectedData] of selectedByGroup) {
        this.g.append('path')
          .datum(groupSelectedData.sort((a: any, b: any) => a.value - b.value))
          .attr('class', 'cdf-line-highlight')
          .attr('d', this.line)
          .attr('fill', 'none')
          .attr('stroke', this.colorScale ? this.colorScale(group) : 'steelblue')
          .style('stroke-width', 1.5);
      }
    }
  }
}

class D3ScatterChart extends BaseChart {
  xColumnName: string;
  yColumnName: string;
  colorBy: string | null;
  xScale: any;
  colorScale: any;

  constructor(container: HTMLElement, attrs: any, mithrilComponent: any) {
    super(container, attrs, mithrilComponent);
    this.xColumnName = attrs.xColumnName;
    this.yColumnName = attrs.yColumnName;
    this.colorBy = attrs.colorBy || null;
    if (this.colorBy) {
      this.margin.right = 180;
    }
  }

  buildQuerySpec(): any {
    return {}; // Scatter uses raw data, no aggregation
  }

  async renderChart() {
    const chartWidth = this.width - this.margin.left - this.margin.right;
    const chartHeight = this.height - this.margin.top - this.margin.bottom;

    this.xScale = this.createLinearScale(
      this.chartData,
      d => +d[this.xColumnName],
      [0, chartWidth]
    );

    this.yScale = this.createLinearScale(
      this.chartData,
      d => +d[this.yColumnName],
      [chartHeight, 0]
    );

    if (this.colorBy) {
      const colorValues = [...new Set(this.chartData.map((d: any) => d[this.colorBy!]))];
      this.colorScale = this.createColorScale(colorValues);
    }

    const brush = this.createBrushHandler('xy', (selection) => {
      const [[x0, y0], [x1, y1]] = selection;

      const selectedData = this.chartData.filter((d: any) => {
        const x = this.xScale(+d[this.xColumnName]);
        const y = this.yScale(+d[this.yColumnName]);
        return x0 <= x && x <= x1 && y0 <= y && y <= y1;
      });

      this.log(`Applying scatter plot filter with ${selectedData.length} selected points`);

      const xValues = selectedData.map(d => +d[this.xColumnName]);
      const yValues = selectedData.map(d => +d[this.yColumnName]);
      const xRange = d3.extent(xValues) as [number, number];
      const yRange = d3.extent(yValues) as [number, number];

      return {
        filters: FilterBuilder.createMultiColumnRange(
          [this.xColumnName, this.yColumnName],
          [xRange, yRange]
        ),
        selectedItems: selectedData
      };
    });

    this.g.append('g')
      .attr('class', 'brush')
      .call(brush);

    const dots = this.g.selectAll('.dot')
      .data(this.chartData)
      .enter().append('circle')
      .attr('class', 'dot selectable')
      .attr('r', 4)
      .attr('cx', (d: any) => this.xScale(+d[this.xColumnName]))
      .attr('cy', (d: any) => this.yScale(+d[this.yColumnName]))
      .style('fill', (d: any) =>
        this.colorBy ? this.colorScale(d[this.colorBy]) : 'steelblue'
      );

    this.addTooltipToSelection(dots, (d: any) => {
      const content: Record<string, any> = {
        [this.xColumnName]: d[this.xColumnName],
        [this.yColumnName]: d[this.yColumnName]
      };
      if (this.colorBy) {
        content[this.colorBy] = d[this.colorBy];
      }
      return content;
    });

    this.createXAxis(this.xScale);
    this.createYAxis(this.yScale);
    this.addGridLines(this.yScale);

    this.drawCorrelationLine();

    if (this.colorBy) {
      this.renderLegend(this.colorScale, this.colorBy);
    }
  }

  drawCorrelationLine() {
    if (!this.g || this.chartData.length < 2) return;

    this.g.selectAll('.correlation-line').remove();
    this.g.selectAll('.correlation-text').remove();

    const { r, slope, intercept } = this.calculateCorrelation(this.chartData);

    const xDomain = this.xScale.domain();
    const x1 = xDomain[0];
    const x2 = xDomain[1];
    const y1 = slope * x1 + intercept;
    const y2 = slope * x2 + intercept;

    this.g.append('line')
      .attr('class', 'correlation-line')
      .attr('x1', this.xScale(x1))
      .attr('y1', this.yScale(y1))
      .attr('x2', this.xScale(x2))
      .attr('y2', this.yScale(y2))
      .attr('stroke', '#666666')
      .attr('stroke-width', 1.5)
      .attr('stroke-dasharray', '5,5')
      .attr('opacity', 0.7);

    const chartWidth = this.width - this.margin.left - this.margin.right;
    this.g.append('text')
      .attr('class', 'correlation-text')
      .attr('x', chartWidth - 10)
      .attr('y', 15)
      .attr('text-anchor', 'end')
      .style('font-size', '12px')
      .style('fill', '#666666')
      .text(`r = ${r.toFixed(3)}`);
  }

  calculateCorrelation(data: any) {
    const n = data.length;
    if (n < 2) return { r: 0, slope: 0, intercept: 0 };

    const xValues = data.map((d: any) => +d[this.xColumnName]);
    const yValues = data.map((d: any) => +d[this.yColumnName]);

    const xMean = d3.mean(xValues) ?? 0;
    const yMean = d3.mean(yValues) ?? 0;

    const numerator = d3.sum(data, (d: any) =>
      (+d[this.xColumnName] - xMean) * (+d[this.yColumnName] - yMean)
    );
    const xSumSquares = d3.sum(xValues, (x: any) => Math.pow(x - xMean, 2));
    const ySumSquares = d3.sum(yValues, (y: any) => Math.pow(y - yMean, 2));

    const denominator = Math.sqrt(xSumSquares * ySumSquares);
    const r = denominator === 0 ? 0 : numerator / denominator;

    const slope = denominator === 0 ? 0 : numerator / xSumSquares;
    const intercept = (yMean ?? 0) - slope * (xMean ?? 0);

    return { r, slope, intercept };
  }
}

class D3HeatmapChart extends BaseChart {
  xColumnName: string;
  yColumnName: string;
  valueColumnName: string;
  aggregationFunction: string;
  xScale: any;
  colorScale: any;

  constructor(container: HTMLElement, attrs: any, mithrilComponent: any) {
    super(container, attrs, mithrilComponent);
    this.xColumnName = attrs.xColumnName;
    this.yColumnName = attrs.yColumnName;
    this.valueColumnName = attrs.valueColumnName;
    this.aggregationFunction = attrs.aggregationFunction || 'sum';
  }

  buildQuerySpec(): any {
    return {
      aggregation: {
        groupBy: [this.xColumnName, this.yColumnName],
        field: this.valueColumnName,
        function: this.aggregationFunction
      }
    };
  }

  async renderChart() {
    const chartWidth = this.width - this.margin.left - this.margin.right;
    const chartHeight = this.height - this.margin.top - this.margin.bottom;

    const yValues = [...new Set(this.chartData.map((d: any) => d[this.yColumnName]))];
    const valueExtent = d3.extent(this.chartData, (d: any) => d.__aggregated_value);

    this.xScale = this.createBandScale(
      this.chartData,
      d => d[this.xColumnName],
      [0, chartWidth],
      0.05
    );

    this.yScale = d3.scaleBand()
      .domain(yValues)
      .range([chartHeight, 0])
      .padding(0.05);

    this.colorScale = d3.scaleSequential(d3.interpolateBlues)
      .domain(valueExtent as [number, number]);

    const brush = this.createBrushHandler('xy', (selection) => {
      const [[x0, y0], [x1, y1]] = selection;

      const selectedData = this.chartData.filter((d: any) => {
        const cellX = this.xScale(d[this.xColumnName]);
        const cellY = this.yScale(d[this.yColumnName]);
        const cellWidth = this.xScale.bandwidth();
        const cellHeight = this.yScale.bandwidth();
        return x0 < cellX + cellWidth && x1 > cellX &&
               y0 < cellY + cellHeight && y1 > cellY;
      });

      this.log(`Applying heatmap filter with ${selectedData.length} selected cells`);

      const xValues = [...new Set(selectedData.map(d => d[this.xColumnName]))];
      const yValues = [...new Set(selectedData.map(d => d[this.yColumnName]))];

      return {
        filters: {
          ...FilterBuilder.createInFilter(this.xColumnName, xValues),
          ...FilterBuilder.createInFilter(this.yColumnName, yValues)
        },
        selectedItems: selectedData
      };
    });

    this.g.append('g')
      .attr('class', 'brush')
      .call(brush);

    const cells = this.g.selectAll('.heatmap-cell')
      .data(this.chartData, (d: any) => d[this.xColumnName] + ':' + d[this.yColumnName])
      .enter()
      .append('rect')
      .attr('class', 'heatmap-cell selectable')
      .attr('x', (d: any) => this.xScale(d[this.xColumnName]))
      .attr('y', (d: any) => this.yScale(d[this.yColumnName]))
      .attr('width', this.xScale.bandwidth())
      .attr('height', this.yScale.bandwidth())
      .style('fill', (d: any) => this.colorScale(d.__aggregated_value));

    this.addTooltipToSelection(cells, (d: any) => ({
      [this.xColumnName]: d[this.xColumnName],
      [this.yColumnName]: d[this.yColumnName],
      [this.valueColumnName]: d.__aggregated_value
    }));

    this.createXAxis(this.xScale);
    this.g.append('g')
      .call(d3.axisLeft(this.yScale).tickFormat((d: any) => truncate(String(d), 10)));
  }
}

class D3BoxplotChart extends BaseChart {
  xColumnName: string;
  yColumnName: string;
  xScale: any;
  clipPathManager!: ClipPathManager;

  constructor(container: HTMLElement, attrs: any, mithrilComponent: any) {
    super(container, attrs, mithrilComponent);
    this.xColumnName = attrs.xColumnName;
    this.yColumnName = attrs.yColumnName;
  }

  buildQuerySpec(): any {
    return {}; // Boxplot uses stats, not query
  }

  async loadData() {
    this.log('Loading boxplot data...');
    this.log('FilterManager active filters:',
      this.filterManager ? this.filterManager.getFilters() : 'No FilterManager');

    const config = {
      filters: this.filterManager ? this.filterManager.getFiltersForQuery() : [],
      valueColumn: this.yColumnName,
      groupByColumn: this.xColumnName
    };

    const result = await this.dataProvider.stats('boxplot', config);
    this.chartData = this.processLoadedData(result.data);

    this.log(`Boxplot data loaded: ${this.chartData.length} groups`);
  }

  async renderChart() {
    const chartContent = this.container.querySelector('.chart-content');
    this.setupSvgWithDimensions(chartContent);
    this.clipPathManager = new ClipPathManager(this.svg);

    const chartWidth = this.width - this.margin.left - this.margin.right;
    const chartHeight = this.height - this.margin.top - this.margin.bottom;

    this.xScale = this.createBandScale(
      this.chartData,
      d => d.key,
      [0, chartWidth],
      0.2
    );

    const allValues = this.chartData.flatMap(d => [d.min, d.max, ...d.outliers]);
    this.yScale = this.createLinearScale(
      [{val: d3.min(allValues)}, {val: d3.max(allValues)}],
      d => d.val,
      [chartHeight, 0]
    );

    const brush = d3.brush()
      .extent([[0, 0], [chartWidth, chartHeight]])
      .on('end', (event: { selection: any; }) => {
        this.handleBrush(event.selection);
      });

    this.g.append('g')
      .attr('class', 'brush')
      .call(brush);

    this.drawBoxplots(this.g, this.chartData, 1.0);

    this.createXAxis(this.xScale);
    this.createYAxis(this.yScale);
    this.addGridLines(this.yScale);
  }

  handleBrush(selection: any) {
    const clippingBrushHelper = new ClippingBrushHelper(this, this.clipPathManager);
    clippingBrushHelper.handleBrush(selection, '.boxplot-group', this.drawBoxplots.bind(this), (sel) => {
      const [[x0, y0], [x1, y1]] = sel;
      const minY = this.yScale.invert(y1);
      const maxY = this.yScale.invert(y0);

      const selectedCategories: string[] = [];
      this.chartData.forEach(d => {
        const categoryX = this.xScale(d.key);
        const categoryWidth = this.xScale.bandwidth();
        if (categoryX !== undefined && x0 < categoryX + categoryWidth && x1 > categoryX) {
          selectedCategories.push(d.key);
        }
      });

      const filters: any = {
        ...FilterBuilder.createRangeFilter(this.yColumnName, minY, maxY)
      };

      if (selectedCategories.length > 0 && selectedCategories.length < this.chartData.length) {
        Object.assign(filters, FilterBuilder.createInFilter(this.xColumnName, selectedCategories));
      }
      return filters;
    });
  }

  drawBoxplots(container: any, data: any[], opacity: number) {
    const boxWidth = this.xScale.bandwidth();

    const boxplotGroups = container.selectAll('.boxplot-group')
      .data(data)
      .enter()
      .append('g')
      .attr('class', 'boxplot-group')
      .attr('transform', (d: any) => `translate(${this.xScale(d.key)}, 0)`)
      .style('opacity', opacity);

    boxplotGroups.append('line')
      .attr('x1', boxWidth / 2)
      .attr('x2', boxWidth / 2)
      .attr('y1', (d: any) => this.yScale(d.min))
      .attr('y2', (d: any) => this.yScale(d.max))
      .attr('stroke', 'black');

    boxplotGroups.append('rect')
      .attr('x', 0)
      .attr('y', (d: any) => this.yScale(d.q3))
      .attr('width', boxWidth)
      .attr('height', (d: any) => this.yScale(d.q1) - this.yScale(d.q3))
      .attr('stroke', 'black')
      .style('fill', 'steelblue');

    boxplotGroups.append('line')
      .attr('x1', 0)
      .attr('x2', boxWidth)
      .attr('y1', (d: any) => this.yScale(d.median))
      .attr('y2', (d: any) => this.yScale(d.median))
      .attr('stroke', 'black')
      .style('stroke-width', 2);

    boxplotGroups.selectAll('.outlier')
      .data((d: any) => d.outliers)
      .enter()
      .append('circle')
      .attr('class', 'outlier')
      .attr('cx', boxWidth / 2)
      .attr('cy', (d: any) => this.yScale(d))
      .attr('r', 3)
      .style('fill', 'red');

    const tooltipManager = TooltipManager.getInstance();
    tooltipManager.addTooltip(boxplotGroups, (d: any) => {
      return `
        <strong>${this.xColumnName}:</strong> ${d.key}<br>
        <strong>Max:</strong> ${d.max.toFixed(2)}<br>
        <strong>Q3:</strong> ${d.q3.toFixed(2)}<br>
        <strong>Median:</strong> ${d.median.toFixed(2)}<br>
        <strong>Q1:</strong> ${d.q1.toFixed(2)}<br>
        <strong>Min:</strong> ${d.min.toFixed(2)}
      `;
    });
  }
}

class D3ViolinPlotChart extends BaseChart {
  xColumnName: string;
  yColumnName: string;
  xScale: any;
  clipPathManager!: ClipPathManager;

  constructor(container: HTMLElement, attrs: any, mithrilComponent: any) {
    super(container, attrs, mithrilComponent);
    this.xColumnName = attrs.xColumnName;
    this.yColumnName = attrs.yColumnName;
  }

  buildQuerySpec(): any {
    return {}; // Violin uses stats, not query
  }

  async loadData() {
    this.log('Loading violin plot data...');
    this.log('FilterManager active filters:',
      this.filterManager ? this.filterManager.getFilters() : 'No FilterManager');

    const config = {
      filters: this.filterManager ? this.filterManager.getFiltersForQuery() : [],
      valueColumn: this.yColumnName,
      groupByColumn: this.xColumnName
    };

    const result = await this.dataProvider.stats('violin', config);
    this.chartData = this.processLoadedData(result.data);

    this.log(`Violin data loaded: ${this.chartData.length} groups`);
  }

  async renderChart() {
    const chartContent = this.container.querySelector('.chart-content');
    this.setupSvgWithDimensions(chartContent);
    this.clipPathManager = new ClipPathManager(this.svg);

    const chartWidth = this.width - this.margin.left - this.margin.right;
    const chartHeight = this.height - this.margin.top - this.margin.bottom;

    this.xScale = this.createBandScale(
      this.chartData,
      d => d.key,
      [0, chartWidth],
      0.05
    );

    // Collect all Y values safely
    const allYValues = this.extractNumericValues(
      this.chartData.flatMap((d: any) => [
        d.min, d.max, d.q1, d.median, d.q3, d.p90, d.p95, d.p99,
        ...(d.density || []).map((p: any) => p[0])
      ]),
      v => v
    );

    const yMin = d3.min(allYValues) ?? 0;
    const yMax = d3.max(allYValues) ?? 100;

    this.yScale = d3.scaleLinear()
      .domain([yMin, yMax])
      .range([chartHeight, 0])
      .nice();

    const brush = d3.brush()
      .extent([[0, 0], [chartWidth, chartHeight]])
      .on('end', (event: { selection: any; }) => {
        this.handleBrush(event.selection);
      });

    this.g.append('g')
      .attr('class', 'brush')
      .call(brush);

    this.drawViolins(this.g, this.chartData, 1.0);

    this.createXAxis(this.xScale);
    this.createYAxis(this.yScale);
    this.addGridLines(this.yScale);
  }

  handleBrush(selection: any) {
    const clippingBrushHelper = new ClippingBrushHelper(this, this.clipPathManager);
    clippingBrushHelper.handleBrush(selection, '.violin-group', this.drawViolins.bind(this), (sel) => {
      const [[x0, y0], [x1, y1]] = sel;
      const minY = this.yScale.invert(y1);
      const maxY = this.yScale.invert(y0);

      const selectedCategories: string[] = [];
      this.chartData.forEach(d => {
        const categoryX = this.xScale(d.key);
        const categoryWidth = this.xScale.bandwidth();
        if (categoryX !== undefined && x0 < categoryX + categoryWidth && x1 > categoryX) {
          selectedCategories.push(d.key);
        }
      });

      const filters: any = {
        ...FilterBuilder.createRangeFilter(this.yColumnName, minY, maxY)
      };

      if (selectedCategories.length > 0 && selectedCategories.length < this.chartData.length) {
        Object.assign(filters, FilterBuilder.createInFilter(this.xColumnName, selectedCategories));
      }
      return filters;
    });
  }

  drawViolins(container: any, data: any[], opacity: number) {
    const densityValues = this.extractNumericValues(
      data.flatMap((d: any) => (d.density || []).map((p: any) => p[1])),
      v => v
    );

    const maxDensity = densityValues.length > 0 ? (d3.max(densityValues) || 0) : 0;

    const xNum = d3.scaleLinear()
      .domain([0, maxDensity])
      .range([0, this.xScale.bandwidth() / 2]);

    const area = d3.area()
      .x0((d: any) => -xNum(d[1]))
      .x1((d: any) => xNum(d[1]))
      .y((d: any) => this.yScale(d[0]))
      .curve(d3.curveCatmullRom);

    const violinGroups = container.selectAll('.violin-group')
      .data(data)
      .enter()
      .append('g')
      .attr('class', 'violin-group')
      .attr('transform', (d: any) =>
        `translate(${this.xScale(d.key) + this.xScale.bandwidth() / 2}, 0)`
      )
      .style('opacity', opacity);

    violinGroups.append('path')
      .datum((d: any) => d.density)
      .attr('d', area)
      .style('fill', 'steelblue');

    violinGroups.append('line')
      .attr('x1', 0)
      .attr('x2', 0)
      .attr('y1', (d: any) => this.yScale(d.q1))
      .attr('y2', (d: any) => this.yScale(d.q3))
      .attr('stroke', 'black')
      .style('stroke-width', 2);

    violinGroups.append('circle')
      .attr('cx', 0)
      .attr('cy', (d: any) => this.yScale(d.median))
      .attr('r', 3)
      .style('fill', 'white');

    violinGroups.append('circle')
      .attr('cx', 0)
      .attr('cy', (d: any) => this.yScale(d.p90))
      .attr('r', 3)
      .style('fill', 'orange');

    violinGroups.append('circle')
      .attr('cx', 0)
      .attr('cy', (d: any) => this.yScale(d.p95))
      .attr('r', 3)
      .style('fill', 'red');

    violinGroups.append('circle')
      .attr('cx', 0)
      .attr('cy', (d: any) => this.yScale(d.p99))
      .attr('r', 3)
      .style('fill', 'purple');

    const tooltipManager = TooltipManager.getInstance();
    tooltipManager.addTooltip(violinGroups, (d: any) => {
      return `
        <strong>${this.xColumnName}:</strong> ${d.key}<br>
        <strong>Median:</strong> ${d.median.toFixed(2)}<br>
        <strong>Q1:</strong> ${d.q1.toFixed(2)}<br>
        <strong>Q3:</strong> ${d.q3.toFixed(2)}<br>
        <strong>P90:</strong> ${d.p90.toFixed(2)}<br>
        <strong>P95:</strong> ${d.p95.toFixed(2)}<br>
        <strong>P99:</strong> ${d.p99.toFixed(2)}
      `;
    });
  }
}

class D3LineChart extends BaseChart {
  xColumnName: string;
  yColumnName: string;
  colorBy: string | null;
  aggregationFunction: string;
  xScale: any;
  colorScale: any;
  line: any;
  clipPathManager!: ClipPathManager;

  constructor(container: HTMLElement, attrs: any, mithrilComponent: any) {
    super(container, attrs, mithrilComponent);
    this.xColumnName = attrs.xColumnName;
    this.yColumnName = attrs.yColumnName;
    this.colorBy = attrs.colorBy || null;
    this.aggregationFunction = attrs.aggregationFunction;
    if (this.colorBy) {
      this.margin.right = 180;
    }
  }

  buildQuerySpec(): any {
    const querySpec: any = {};
    if (this.aggregationFunction) {
      const groupBy = [this.xColumnName];
      if (this.colorBy) {
        groupBy.push(this.colorBy);
      }
      querySpec.aggregation = {
        groupBy: groupBy,
        field: this.yColumnName,
        function: this.aggregationFunction
      };
    }
    return querySpec;
  }

  async renderChart() {
    const chartContent = this.container.querySelector('.chart-content');
    this.setupSvgWithDimensions(chartContent);
    this.clipPathManager = new ClipPathManager(this.svg);

    const chartWidth = this.width - this.margin.left - this.margin.right;
    const chartHeight = this.height - this.margin.top - this.margin.bottom;

    const yValueAccessor = (d: any) =>
      this.aggregationFunction ? d.__aggregated_value : +d[this.yColumnName];

    this.xScale = this.createLinearScale(
      this.chartData,
      d => +d[this.xColumnName],
      [0, chartWidth]
    );

    this.yScale = this.createLinearScale(
      this.chartData,
      yValueAccessor,
      [chartHeight, 0]
    );

    this.line = d3.line()
      .x((d: any) => this.xScale(+d[this.xColumnName]))
      .y((d: any) => this.yScale(yValueAccessor(d)));

    if (this.colorBy) {
      const colorValues = [...new Set(this.chartData.map((d: any) => d[this.colorBy!]))];
      this.colorScale = this.createColorScale(colorValues);
    }

    const brush = d3.brushX()
      .extent([[0, 0], [chartWidth, chartHeight]])
      .on('end', (event: { selection: any; }) => {
        this.handleBrush(event.selection);
      });

    this.g.append('g')
      .attr('class', 'brush')
      .call(brush);

    this.drawLines(this.g, this.chartData, 1.0);

    this.createXAxis(this.xScale);
    this.createYAxis(this.yScale);
    this.addGridLines(this.yScale);

    if (this.colorBy) {
      this.renderLegend(this.colorScale, this.colorBy);
    }
  }

  handleBrush(selection: any) {
    const clippingBrushHelper = new ClippingBrushHelper(this, this.clipPathManager);
    clippingBrushHelper.handleBrush(selection, '.line', this.drawLines.bind(this), (sel) => {
      const [x0, x1] = sel;
      const minX = this.xScale.invert(x0);
      const maxX = this.xScale.invert(x1);

      return FilterBuilder.createRangeFilter(
        this.xColumnName,
        minX,
        maxX,
      );
    });
  }

  drawLines(container: any, data: any[], opacity: number) {
    if (this.colorBy) {
      const groupedData = d3.group(data, (d: any) => d[this.colorBy!]);
      groupedData.forEach((groupData: any, key: any) => {
        container.append('path')
          .datum(groupData)
          .attr('class', 'line')
          .attr('fill', 'none')
          .attr('stroke', this.colorScale(key))
          .attr('stroke-width', 1.5)
          .attr('d', this.line)
          .style('opacity', opacity);
      });
    } else {
      container.append('path')
        .datum(data)
        .attr('class', 'line')
        .attr('fill', 'none')
        .attr('stroke', 'steelblue')
        .attr('stroke-width', 1.5)
        .attr('d', this.line)
        .style('opacity', opacity);
    }
  }
}

class D3DonutChart extends BaseChart {
  valueColumnName: string;
  categoryColumnName: string;
  aggregationFunction: string;
  colorScale: any;

  constructor(container: HTMLElement, attrs: any, mithrilComponent: any) {
    super(container, attrs, mithrilComponent);
    this.valueColumnName = attrs.valueColumnName;
    this.categoryColumnName = attrs.categoryColumnName;
    this.aggregationFunction = attrs.aggregationFunction || 'sum';
    this.margin = {top: 10, right: 180, bottom: 90, left: 60};
    if (!this.categoryColumnName) {
      this.margin.right = 30;
    }
  }

  buildQuerySpec(): any {
    return {
      aggregation: {
        groupBy: this.categoryColumnName,
        field: this.valueColumnName,
        function: this.aggregationFunction
      }
    };
  }

  async renderChart() {
    const chartWidth = this.width - this.margin.left - this.margin.right;
    const chartHeight = this.height - this.margin.top - this.margin.bottom;
    const radius = Math.min(chartWidth, chartHeight) / 2 - 10;

    const pie = d3.pie()
      .value((d: any) => d.__aggregated_value)
      .sort(null);

    const arc = d3.arc()
      .innerRadius(radius * 0.5)
      .outerRadius(radius);

    const categories = this.chartData.map((d: any) => d[this.categoryColumnName]);
    this.colorScale = this.createColorScale(categories);

    const arcs = this.g.selectAll('.arc')
      .data(pie(this.chartData))
      .enter()
      .append('g')
      .attr('class', 'arc')
      .attr('transform', `translate(${chartWidth / 2}, ${chartHeight / 2})`);

    const paths = arcs.append('path')
      .attr('d', arc)
      .attr('fill', (d: any) => this.colorScale(d.data[this.categoryColumnName]))
      .on('click', (event: any, d: any) => {
        event.stopPropagation();
        const currentSlice = d3.select(event.currentTarget.parentNode);
        const isSelected = currentSlice.classed('selected');

        if (!event.shiftKey) {
          this.g.selectAll('.arc').classed('selected', false).style('opacity', 0.2);
        }

        currentSlice.classed('selected', !isSelected).style('opacity', 1.0);

        if (this.g.selectAll('.arc.selected').empty()) {
          this.g.selectAll('.arc').style('opacity', 1.0);
        }

        if (this.filterManager && !isSelected) {
          this.log('Applying donut chart filter for clicked slice');
          const categoryValue = d.data[this.categoryColumnName];
          this.filterManager.setFilters(
            FilterBuilder.createInFilter(this.categoryColumnName, [categoryValue]),
            this.mithrilComponent
          );
        } else if (this.filterManager && isSelected) {
          this.filterManager.clearFiltersForChart(this.mithrilComponent);
        }
      });

    this.svg.on('click', (event: any) => {
      if (event.target === this.svg.node()) {
        this.g.selectAll('.arc').classed('selected', false).style('opacity', 1.0);
        if (this.filterManager) {
          this.filterManager.clearFiltersForChart(this.mithrilComponent);
        }
      }
    });

    this.addTooltipToSelection(paths, (d: any) => ({
      [this.categoryColumnName]: d.data[this.categoryColumnName],
      [this.valueColumnName]: d.data.__aggregated_value
    }));

    this.renderLegend(this.colorScale, this.categoryColumnName);
  }
}

class D3StackedBarChart extends BaseChart {
  xColumnName: string;
  yColumnName: string;
  stackColumnName: string;
  aggregationFunction: string;
  xScale: any;
  colorScale: any;

  constructor(container: HTMLElement, attrs: any, mithrilComponent: any) {
    super(container, attrs, mithrilComponent);
    this.xColumnName = attrs.xColumnName;
    this.yColumnName = attrs.yColumnName;
    this.stackColumnName = attrs.stackColumnName;
    this.aggregationFunction = attrs.aggregationFunction;
    if (this.stackColumnName) {
      this.margin.right = 180;
    }
  }

  buildQuerySpec(): any {
    return {
      aggregation: {
        groupBy: [this.xColumnName, this.stackColumnName],
        field: this.yColumnName,
        function: this.aggregationFunction
      }
    };
  }

  processLoadedData(data: any[]): any[] {
    const stackKeys = [...new Set(data.map(d => d[this.stackColumnName]))].sort();
    const stackDataMap = new Map();

    data.forEach(item => {
      const xValue = item[this.xColumnName];
      const stackValue = item[this.stackColumnName];
      const aggregatedValue = item.__aggregated_value || 0;

      if (!stackDataMap.has(xValue)) {
        stackDataMap.set(xValue, {
          [this.xColumnName]: xValue
        });
        stackKeys.forEach(key => {
          stackDataMap.get(xValue)[key] = 0;
        });
      }

      stackDataMap.get(xValue)[stackValue] = Math.max(0, aggregatedValue);
    });

    return Array.from(stackDataMap.values());
  }

  async renderChart() {
    const chartWidth = this.width - this.margin.left - this.margin.right;
    const chartHeight = this.height - this.margin.top - this.margin.bottom;

    const keys = Object.keys(this.chartData[0]).filter(k => k !== this.xColumnName);
    const stack = d3.stack().keys(keys);
    const series = stack(this.chartData);

    this.xScale = this.createBandScale(
      this.chartData,
      d => d[this.xColumnName],
      [0, chartWidth]
    );

    this.yScale = d3.scaleLinear()
      .domain([0, d3.max(series, (d: any) => d3.max(d, (d: any) => +d[1])) || 0])
      .range([chartHeight, 0])
      .nice();

    this.colorScale = this.createColorScale(keys);

    const brush = this.createBrushHandler('xy', (selection) => {
      const [[x0, y0], [x1, y1]] = selection;

      const selectedData: any[] = [];
      this.g.selectAll('rect').each((d: any) => {
        if (!d || !d.data || !d.data[this.xColumnName]) return;

        const barX = this.xScale(d.data[this.xColumnName]);
        const barY = this.yScale(d[1]);
        const barWidth = this.xScale.bandwidth();
        const barHeight = this.yScale(d[0]) - this.yScale(d[1]);
        const isBrushed = x0 < barX + barWidth && x1 > barX &&
                         y0 < barY + barHeight && y1 > barY;
        if (isBrushed) {
          selectedData.push(d);
        }
      });

      if (selectedData.length === 0) {
        return { filters: {}, selectedItems: [] };
      }

      this.log(`Applying stacked bar chart brush filter with ${selectedData.length} selected bars`);

      const xValues = [...new Set(selectedData.map(d => d.data[this.xColumnName]))];
      const sampleValue = xValues.length > 0 ? xValues[0] : null;
      const isNumeric = sampleValue !== null && !isNaN(Number(sampleValue));

      let filters: any = {};

      if (isNumeric) {
        const numericValues = xValues.map(v => Number(v)).sort((a, b) => a - b);
        const minValue = numericValues[0];
        const maxValue = numericValues[numericValues.length - 1];
        filters = FilterBuilder.createRangeFilter(this.xColumnName, minValue, maxValue);
      } else {
        filters = FilterBuilder.createInFilter(this.xColumnName, xValues);
      }

      return {
        filters: filters,
        selectedItems: selectedData
      };
    });

    this.g.append('g')
      .attr('class', 'brush')
      .call(brush);

    const barGroups = this.g.selectAll('.bar-group')
      .data(series)
      .enter().append('g')
      .attr('class', 'bar-group')
      .attr('fill', (d: any) => this.colorScale(d.key));

    const bars = barGroups.selectAll('rect')
      .data((d: any) => d)
      .enter().append('rect')
      .attr('class', 'selectable')
      .attr('x', (d: any) => this.xScale(d.data[this.xColumnName]))
      .attr('y', (d: any) => this.yScale(d[1]))
      .attr('height', (d: any) => this.yScale(d[0]) - this.yScale(d[1]))
      .attr('width', this.xScale.bandwidth())
      .on('click', (event: any, d: any) => {
        event.stopPropagation();
        const currentBar = d3.select(event.currentTarget);
        const isSelected = currentBar.classed('selected');

        if (!event.shiftKey) {
          this.g.selectAll('rect').classed('selected', false).style('opacity', 0.2);
        }

        currentBar.classed('selected', !isSelected).style('opacity', 1.0);

        if (this.g.selectAll('rect.selected').empty()) {
          this.g.selectAll('rect').style('opacity', 1.0);
        }

        if (this.filterManager && !isSelected) {
          this.log('Applying stacked bar chart filter for clicked bar');

          const xValue = d.data[this.xColumnName];
          const stackValue = (d3.select(event.currentTarget.parentNode).datum() as any).key;

          const filters = {
            ...FilterBuilder.createInFilter(this.xColumnName, [xValue]),
            ...FilterBuilder.createInFilter(this.stackColumnName, [stackValue])
          };

          this.filterManager.setFilters(filters, this.mithrilComponent);
        } else if (this.filterManager && isSelected) {
          this.filterManager.clearFiltersForChart(this.mithrilComponent);
        }
      });

    this.addTooltipToSelection(bars, (d: any, el: any) => {
      const parentNode = el.parentNode;
      const stackValue = d3.select(parentNode).datum() as any;
      return {
        [this.xColumnName]: d.data[this.xColumnName],
        [this.stackColumnName]: stackValue.key,
        [this.yColumnName]: d.data[stackValue.key]
      };
    });

    this.createXAxis(this.xScale);
    this.createYAxis(this.yScale);
    this.addGridLines(this.yScale);

    this.renderLegend(this.colorScale, this.stackColumnName);
  }
}

class D3AreaChart extends BaseChart {
  xColumnName: string;
  yColumnName: string;
  stackColumnName: string;
  aggregationFunction: string;
  xScale: any;
  colorScale: any;
  clipPathManager!: ClipPathManager;

  constructor(container: HTMLElement, attrs: any, mithrilComponent: any) {
    super(container, attrs, mithrilComponent);
    this.xColumnName = attrs.xColumnName;
    this.yColumnName = attrs.yColumnName;
    this.stackColumnName = attrs.stackColumnName;
    this.aggregationFunction = attrs.aggregationFunction;
    if (this.stackColumnName) {
      this.margin.right = 180;
    }
  }

  buildQuerySpec(): any {
    if (this.stackColumnName) {
      return {
        aggregation: {
          groupBy: [this.xColumnName, this.stackColumnName],
          field: this.yColumnName,
          function: this.aggregationFunction
        }
      };
    } else if (this.aggregationFunction) {
      return {
        aggregation: {
          groupBy: this.xColumnName,
          field: this.yColumnName,
          function: this.aggregationFunction
        }
      };
    }
    return {};
  }

  processLoadedData(data: any[]): any[] {
    if (this.stackColumnName) {
      const stackKeys = [...new Set(data.map(d => d[this.stackColumnName]))].sort();
      const stackDataMap = new Map();

      data.forEach(item => {
        const xValue = item[this.xColumnName];
        const stackValue = item[this.stackColumnName];
        const aggregatedValue = item.__aggregated_value || 0;

        if (!stackDataMap.has(xValue)) {
          stackDataMap.set(xValue, {
            [this.xColumnName]: xValue
          });
          stackKeys.forEach(key => {
            stackDataMap.get(xValue)[key] = 0;
          });
        }

        stackDataMap.get(xValue)[stackValue] = Math.max(0, aggregatedValue);
      });

      return Array.from(stackDataMap.values()).sort((a, b) => {
        const valA = a[this.xColumnName];
        const valB = b[this.xColumnName];
        if (typeof valA === 'string' && typeof valB === 'string') {
          return valA.localeCompare(valB);
        }
        return valA - valB;
      });
    } else {
      return data;
    }
  }

  async renderChart() {
    const chartContent = this.container.querySelector('.chart-content');
    this.setupSvgWithDimensions(chartContent);
    this.clipPathManager = new ClipPathManager(this.svg);

    const chartWidth = this.width - this.margin.left - this.margin.right;
    const chartHeight = this.height - this.margin.top - this.margin.bottom;

    let stackedSeries = null;
    if (this.stackColumnName) {
      const keys = Object.keys(this.chartData[0]).filter(k => k !== this.xColumnName);
      const stack = d3.stack().keys(keys);
      stackedSeries = stack(this.chartData);
    }

    const allXData = this.chartData.map((d: any) => d[this.xColumnName]);

    this.xScale = d3.scaleBand()
      .domain(allXData)
      .range([0, chartWidth])
      .padding(0.1);

    let yExtent;
    if (stackedSeries) {
      const maxStackedValue = d3.max(stackedSeries, (series: any) =>
        d3.max(series, (d: any) => d[1])
      ) || 0;
      yExtent = [0, maxStackedValue];
    } else {
      const maxY = d3.max(this.chartData, (d: any) =>
        Math.max(0, d.__aggregated_value || d[this.yColumnName])
      ) || 0;
      yExtent = [0, maxY];
    }

    this.yScale = d3.scaleLinear()
      .domain(yExtent as [number, number])
      .range([chartHeight, 0])
      .nice()
      .clamp(true);

    if (this.stackColumnName) {
      const colorValues = stackedSeries ? stackedSeries.map((s: any) => s.key) : [];
      this.colorScale = this.createColorScale(colorValues);
    } else {
      this.colorScale = d3.scaleOrdinal().range(['steelblue']);
    }

    const brush = d3.brushX()
      .extent([[0, 0], [chartWidth, chartHeight]])
      .on('end', (event: { selection: any; }) => {
        this.handleBrush(event.selection, stackedSeries);
      });

    this.g.append('g')
      .attr('class', 'brush')
      .call(brush);

    if (stackedSeries) {
      this.drawStackedAreas(stackedSeries);
    } else {
      this.drawSimpleArea();
    }

    this.createXAxis(this.xScale);
    this.createYAxis(this.yScale);
    this.addGridLines(this.yScale);

    if (this.stackColumnName) {
      this.renderLegend(this.colorScale, this.stackColumnName);
    }
  }

  handleBrush(selection: any, stackedSeries: any) {
    const clippingBrushHelper = new ClippingBrushHelper(this, this.clipPathManager);
    const drawCallback = stackedSeries ?
      (container: any, _data: any[], opacity: number) => this.drawStackedAreasInGroup(container, stackedSeries, opacity) :
      (container: any, _data: any[], opacity: number) => this.drawSimpleAreaInGroup(container, opacity);

    clippingBrushHelper.handleBrush(selection, '.area', drawCallback, (sel) => {
      const [x0, x1] = sel;
      const selectedCategories: any[] = [];
      this.xScale.domain().forEach((category: any) => {
        const categoryPos = this.xScale(category)! + this.xScale.bandwidth() / 2;
        if (categoryPos >= x0 && categoryPos <= x1) {
          selectedCategories.push(category);
        }
      });

      if (selectedCategories.length > 0 && typeof selectedCategories[0] === 'number') {
        const extent = d3.extent(selectedCategories) as [number, number];
        return FilterBuilder.createRangeFilter(this.xColumnName, extent[0], extent[1]);
      }
      return FilterBuilder.createInFilter(this.xColumnName, selectedCategories);
    });
  }

  drawStackedAreas(series: any, opacity: number = 1.0) {
    this.drawStackedAreasInGroup(this.g, series, opacity);
  }

  drawStackedAreasInGroup(container: any, series: any, opacity: number) {
    const area = d3.area()
      .x((d: any) => this.xScale(d.data[this.xColumnName]) + this.xScale.bandwidth() / 2)
      .y0((d: any) => Math.max(0, this.yScale(d[0])))
      .y1((d: any) => Math.max(0, this.yScale(d[1])))
      .curve(d3.curveBasis);

    container.selectAll('.area')
      .data(series)
      .enter().append('path')
      .attr('class', 'area')
      .attr('d', area)
      .style('fill', (d: any) => this.colorScale(d.key))
      .style('opacity', opacity);
  }

  drawSimpleArea(opacity: number = 1.0) {
    this.drawSimpleAreaInGroup(this.g, opacity);
  }

  drawSimpleAreaInGroup(container: any, opacity: number) {
    const area = d3.area()
      .x((d: any) => this.xScale(d[this.xColumnName]) + this.xScale.bandwidth() / 2)
      .y0(this.yScale(0))
      .y1((d: any) => Math.max(this.yScale(0),
        this.yScale(d.__aggregated_value || d[this.yColumnName])
      ))
      .curve(d3.curveBasis);

    container.append('path')
      .datum(this.chartData)
      .attr('class', 'area')
      .attr('d', area)
      .style('fill', 'steelblue')
      .style('opacity', opacity);
  }
}

export class DataTableComponent implements m.Component<{
    dataProvider: DataGridDataSource,
    columns: ColumnDefinition[],
    filterManager?: any,
}> {
  dom: HTMLElement|null = null;
  dataGrid: DataGrid|null = null;
  vnode: m.Vnode<{
    dataProvider: DataGridDataSource,
    columns: ColumnDefinition[],
    filterManager?: any,
  }>|null = null;

  oncreate(vnode: m.VnodeDOM<{
    dataProvider: DataGridDataSource,
    columns: ColumnDefinition[],
    filterManager?: any,
  }>) {
    this.vnode = vnode;
    this.dom = vnode.dom as HTMLElement;
    const chartId = `d3-data-table-${Math.random().toString(36).substr(2, 9)}`;
    this.dom.id = chartId;

    if (vnode.attrs.filterManager) {
      vnode.attrs.filterManager.subscribe(this);
    }
    ChartManager.register(this);
  }

  async refreshFilteredData() {
    const chartId = this.dom?.id || 'unknown';
    console.log(`[${chartId}] DataTableComponent.refreshFilteredData called`);

    if (this.dataGrid && this.vnode && this.vnode.attrs.filterManager) {
      const filters = this.vnode.attrs.filterManager.getFilters();
      console.log(`[${chartId}] Applying filters to data grid:`, filters);

      // Convert internal filter format to DataGrid format
      const filterDefs = FilterConverter.toFilterDefinitions(filters);
      // TODO(zezeozue): This is a temporary hack to get around the fact that
      // the data grid doesn't support controlled filters yet.
      (this.dataGrid as any).filters = filterDefs;
    }

    m.redraw();
  }

  setIsFilterSource(isSource: boolean) {
    const chartId = this.dom?.id || 'unknown';
    console.log(`[${chartId}] DataTableComponent.setIsFilterSource called with: ${isSource}`);
  }


  onremove(vnode: any) {
    if (vnode.attrs.filterManager) {
      vnode.attrs.filterManager.unsubscribe(this);
    }
    ChartManager.unregister(this);
  }

  view(vnode: m.Vnode<{
    dataProvider: DataGridDataSource,
    columns: ColumnDefinition[],
    filterManager?: any,
  }>): m.Children {
    this.vnode = vnode;
    return m(
        '.chart-container',
        {
          style: {
            height: '400px',
            overflow: 'auto',
            minWidth: 0,
          },
        },
        m('.chart-header',
          {
            onclick: (event: MouseEvent) => {
              event.stopPropagation();
              if (vnode.attrs.filterManager) {
                vnode.attrs.filterManager.clearFiltersForChart(this);
              }
            },
          },
          m('h4.chart-title', 'Data Table'),
          m('.chart-actions',
            m('button.chart-action-btn', {title: 'Duplicate chart', onclick: () => ChartManager.duplicateChart(this.dom)}, '⧉'),
            m('button.chart-close-btn', {title: 'Remove Chart', onclick: () => ChartManager.removeChart(this.dom)}, '×'),
            ),
          ),
        m('.datagrid-container',
          m(DataGrid, {
            fillHeight: true,
            data: vnode.attrs.dataProvider,
            columns: vnode.attrs.columns,
            oncreate: (vnode: m.VnodeDOM<DataGridAttrs, DataGrid>) => {
              this.dataGrid = vnode.state as DataGrid;
              this.refreshFilteredData();
            },
            onFiltersChanged: (filters: ReadonlyArray<DataGridFilter>) => {
              this.handleFiltersChanged(filters, vnode);
            },
            filters: vnode.attrs.filterManager ? FilterConverter.toFilterDefinitions(vnode.attrs.filterManager.getFilters()) : [],
          } as any),
        ),
        m('.resize-handle', {
          onmousedown: (e: MouseEvent) => {
            if (this.dom) {
              const chartContainer = this.dom.closest('.chart-container');
              if (chartContainer) {
                ResizeManager.getInstance().onMouseDown(
                    e, chartContainer as HTMLElement, chartContainer as HTMLElement);
              }
            }
          },
        }),
    );
  }

  private handleFiltersChanged(
    filters: ReadonlyArray<DataGridFilter>,
    vnode: m.Vnode<any>
  ) {
    const chartId = this.dom?.id || 'unknown';
    console.log(`[${chartId}] DataTableComponent.onFiltersChanged called with:`, filters);

    if (!vnode.attrs.filterManager) return;

    const currentFilters = vnode.attrs.filterManager.getFilters();
    const { filterMap, clearedColumns } = FilterConverter.fromFilterDefinitions(
      filters,
      currentFilters
    );

    // Handle cleared or modified filters
    if (filters.length === 0 || clearedColumns.size > 0) {
      vnode.attrs.filterManager.clearFiltersForChart(this, undefined, true);
      return;
    }

    // Apply new filters
    if (Object.keys(filterMap).length > 0) {
      vnode.attrs.filterManager.setFilters(filterMap, this);
    }
  }
}

export const D3BarChartComponent = new ChartComponent(D3BarChart);

export const D3HistogramComponent = new ChartComponent(D3HistogramChart);

export const D3CDFComponent = new ChartComponent(D3CDFChart);

export const D3ScatterChartComponent = new ChartComponent(D3ScatterChart);

export const D3HeatmapChartComponent = new ChartComponent(D3HeatmapChart);

export const D3BoxplotChartComponent = new ChartComponent(D3BoxplotChart);

export const D3ViolinPlotChartComponent = new ChartComponent(D3ViolinPlotChart);

export const D3LineChartComponent = new ChartComponent(D3LineChart);

export const D3DonutChartComponent = new ChartComponent(D3DonutChart);

export const D3StackedBarChartComponent = new ChartComponent(D3StackedBarChart);

export const D3AreaChartComponent = new ChartComponent(D3AreaChart);


