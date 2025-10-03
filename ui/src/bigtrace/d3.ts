import m from 'mithril';
import * as d3 from 'd3';

// --- Duplicated from framework.ts ---
class ComponentManager {
  static components: any[] = [];

  static register(component: any): any {
    if (this.components.includes(component)) {
      return component;
    }
    this.components.push(component);
    return component;
  }
}

class ChartManager {
  static charts: any[] = [];

  static register(chartComponent: any): void {
    if (!this.charts.includes(chartComponent)) {
      this.charts.push(chartComponent);
    }
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

    // Find the chart component
    const chartComponent = this.charts.find(c => c.dom === chartDomElement);
    if (!chartComponent) return;

    if (chartComponent instanceof DataTableComponent) {
      (window as any).populateChartCreator?.('table', {});
      return;
    }

    if (!chartComponent.chart) return;

    const chart = chartComponent.chart;

    // Extract configuration based on chart type
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
      // Populate the chart creator sidebar (this is in main.ts)
      // We'll need to import or access chartCreatorState
      (window as any).populateChartCreator?.(chartType, config);
    }
  }
}

export class ResizeManager {
  private isResizing: boolean = false;
  private currentContainer: HTMLElement | null = null;
  private startX: number = 0;
  private startY: number = 0;
  private startWidth: number = 0;
  private startHeight: number = 0;

  private static instance: ResizeManager;

  private constructor() {
    this.onMouseMove = this.onMouseMove.bind(this);
    this.onMouseUp = this.onMouseUp.bind(this);
  }

  public static getInstance(): ResizeManager {
    if (!ResizeManager.instance) {
      ResizeManager.instance = new ResizeManager();
    }
    return ResizeManager.instance;
  }

  public onMouseDown(
    e: MouseEvent, chartContainer: HTMLElement, container: HTMLElement) {
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
    const newWidth = Math.max(minWidth, this.startWidth + deltaX);
    const newHeight = Math.max(minHeight, this.startHeight + deltaY);

    const numColumns = Math.round(newWidth / minWidth);
    const gridColumn = `span ${Math.max(1, numColumns)}`;
    if (this.currentContainer.style.gridColumn !== gridColumn) {
      this.currentContainer.style.gridColumn = gridColumn;
    }

    this.currentContainer.style.height = newHeight + 'px';
    this.redrawChart(this.currentContainer);
  }

  private onMouseUp(): void {
    if (this.isResizing) {
      if (this.currentContainer) {
        this.currentContainer.style.cursor = '';
        this.currentContainer.style.userSelect = '';
      }
      this.isResizing = false;
      this.currentContainer = null;
      window.removeEventListener('mousemove', this.onMouseMove);
      window.removeEventListener('mouseup', this.onMouseUp);
    }
  }

  private redrawChart(container: HTMLElement): void {
    const chartWrapper = ComponentManager.components.find(
      (comp) => comp.dom === container
    );
    if (chartWrapper && chartWrapper.chart) {
      chartWrapper.chart.redrawWithoutRefetch();
    }
  }
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

// --- From memory_data_provider.ts ---
export class MemoryDataProvider extends DataProvider {
  data: any[];
  constructor(data: any[]) {
    super('memory');
    this.data = data;
  }
  async query(querySpec: any): Promise<any> {
    let result = [...this.data];

    // Apply filters
    if (querySpec.filters) {
      result = result.filter(row => {
        return querySpec.filters.every((filter: any) => {
          const cellValue = row[filter.column];

          switch (filter.type) {
            case 'IN':
              const values = new Set(filter.value);
              return values.has(String(cellValue));
           case 'LIKE':
               return String(cellValue).toLowerCase().includes(String(filter.value).toLowerCase());
            case 'RANGE':
              const numValue = Number(cellValue);
              switch (filter.operator) {
                case '>': return numValue > filter.value;
                case '>=': return numValue >= filter.value;
                case '<': return numValue < filter.value;
                case '<=': return numValue <= filter.value;
                case '=': return numValue === filter.value;
                default: return true;
              }
            default:
              return true;
          }
        });
      });
    }

    if (querySpec.aggregation) {
        result = this.applyAggregation(result, querySpec.aggregation);
    }
    return {data: result};
  }
  applyAggregation(data: any[], aggregation: any) {
    const groupBy = Array.isArray(aggregation.groupBy) ? aggregation.groupBy : [aggregation.groupBy];
    const grouped = d3.group(data, ...groupBy.map((field: any) => (d: any) => d[field]));

    const aggregatedData: any[] = [];

    function recurse(currentGroup: any, groupKeys: any[]) {
        if (currentGroup instanceof Map) {
            for (const [key, value] of currentGroup) {
                recurse(value, [...groupKeys, key]);
            }
        } else {
            let aggregatedValue;
            const numericValues = currentGroup.map((d: any) => +d[aggregation.field]).filter((v: any) => !isNaN(v));
            switch (aggregation.function) {
                case 'sum':
                    aggregatedValue = d3.sum(numericValues);
                    break;
                case 'mean':
                    aggregatedValue = d3.mean(numericValues);
                    break;
                case 'count':
                    aggregatedValue = numericValues.length;
                    break;
                case 'min':
                    aggregatedValue = d3.min(numericValues);
                    break;
                case 'max':
                    aggregatedValue = d3.max(numericValues);
                    break;
                default:
                    aggregatedValue = d3.sum(numericValues);
            }
            const aggregatedPoint: any = {
                __aggregated_value: aggregatedValue,
            };
            groupBy.forEach((field: any, i: number) => {
                aggregatedPoint[field] = groupKeys[i];
            });
            aggregatedData.push(aggregatedPoint);
        }
    }

    recurse(grouped, []);
    return aggregatedData;
  }
}

// --- Simple D3BarChart class ---
class D3BarChart {
  container: HTMLElement;
  width: number = 600;
  height: number = 400;
  margin = { top: 10, right: 30, bottom: 150, left: 60 };
  svg: any;
  g: any;
  chartData: any[] = [];
  xColumnName: string;
  yColumnName: string;
  dataProvider: any;
  aggregationFunction: string;
  resizeTimeout: any;
  filterManager: any;
  isFilterSource: boolean = false;

  constructor(container: HTMLElement, attrs: any) {
    this.container = container;
    this.dataProvider = attrs.dataProvider;
    this.filterManager = attrs.filterManager; // Store FilterManager reference
    this.xColumnName = attrs.xColumnName;
    this.yColumnName = attrs.yColumnName;
    this.aggregationFunction = attrs.aggregationFunction;
  }

  getContainerDimensions() {
    const rect = this.container.getBoundingClientRect();
    return {
      width: rect.width || 600,
      height: rect.height || 400,
    };
  }

  async loadData() {
    console.log(`[${this.container.id}] Loading data...`);
    console.log(`[${this.container.id}] FilterManager active filters:`, this.filterManager ? this.filterManager.getFilters() : 'No FilterManager');

    const querySpec = {
      aggregation: {
        groupBy: this.xColumnName,
        field: this.yColumnName,
        function: this.aggregationFunction
      }
    };

    const querySpecWithFilters = {
      ...querySpec,
      filters: this.filterManager ? this.filterManager.getFiltersForQuery() : [],
    };
    const result = await this.dataProvider.query(querySpecWithFilters);
    this.chartData = result.data;

    console.log(`[${this.container.id}] Data loaded:`, this.chartData);
    console.log(`[${this.container.id}] Aggregated chart data length:`, this.chartData.length);
  }

  transformFilter(column: any, filter: any) {
    return {
      column,
      type: filter.type,
      operator: filter.operator,
      value: filter.values ? Array.from(filter.values) : filter.value,
      negate: filter.negate || false
    };
  }

  setupSvgWithDimensions(chartContent: any) {
    if (!chartContent) return null;

    // Clear everything first
    d3.select(chartContent).selectAll("*").remove();

    const containerDimensions = this.getContainerDimensions();
    const minBarWidth = 20;
    const calculatedWidth = this.chartData.length * minBarWidth;
    const chartWidth = Math.max(
      containerDimensions.width - this.margin.left - this.margin.right,
      calculatedWidth,
    );

    this.width = chartWidth + this.margin.left + this.margin.right;
    this.height = containerDimensions.height;

    this.svg = d3.select(chartContent)
      .append('svg')
      .attr('width', this.width)
      .attr('height', this.height);

    this.g = this.svg.append('g').attr(
      'transform', `translate(${this.margin.left},${this.margin.top})`
    );

    return this.svg;
  }

  async render() {
    console.log(`[${this.container.id}] Rendering chart...`);
    await this.loadData();

    const chartContent = this.container.querySelector('.chart-content');
    if (!chartContent) {
      console.error(`[${this.container.id}] Chart content area not found.`);
      return;
    }

    this.setupSvgWithDimensions(chartContent);

    // Handle empty data case
    if (!this.chartData || this.chartData.length === 0) {
      console.log(`[${this.container.id}] No data to render, skipping chart rendering`);
      // Add a "No data" message
      this.g.append('text')
        .attr('x', (this.width - this.margin.left - this.margin.right) / 2)
        .attr('y', (this.height - this.margin.top - this.margin.bottom) / 2)
        .attr('text-anchor', 'middle')
        .style('font-size', '14px')
        .style('fill', '#666')
        .text('No data available');
      return;
    }

    // Setup scales
    const xScale = d3.scaleBand()
      .domain(this.chartData.map(d => d[this.xColumnName]))
      .range([0, this.width - this.margin.left - this.margin.right])
      .padding(0.1);

    const yScale = d3.scaleLinear()
      .domain([0, d3.max(this.chartData, (d: any) => d.__aggregated_value) || 0])
      .nice()
      .range([this.height - this.margin.top - this.margin.bottom, 0]);

    // Create axes
    const xAxis = d3.axisBottom(xScale);
    const yAxis = d3.axisLeft(yScale).tickFormat((d: any) => truncate(String(d), 10));

    this.g.append('g')
      .attr('transform', `translate(0,${this.height - this.margin.top - this.margin.bottom})`)
      .call(xAxis)
      .selectAll('text')
      .style('text-anchor', 'end')
      .attr('dx', '-.8em')
      .attr('dy', '.15em')
      .attr('transform', 'rotate(-45)')
      .text((d: any, i: number) => {
        if (i % 2 !== 0) return '';
        return truncate(String(d), 10);
      });

    this.g.append('g')
      .call(yAxis);

    // Add brush for selection
    const brush = d3.brushX()
      .extent([[0, 0], [this.width - this.margin.left - this.margin.right, this.height - this.margin.top - this.margin.bottom]])
      .on('end', (event: { selection: any; }) => {
        console.log(`[${this.container.id}] Bar chart brush event ended.`);
        if (!event.selection) {
          console.log(`[${this.container.id}] No selection, clearing filters and resetting opacity.`);
          // Reset all bars to full opacity when no selection
          this.g.selectAll('.bar').style('opacity', 1.0);
          if (this.filterManager) {
            this.filterManager.clearFiltersForChart(this);
          }
          return;
        }

        const [x0, x1] = event.selection;
        const selectedData = this.chartData.filter((d: any) => {
          const barX = xScale(d[this.xColumnName]);
          if (barX === undefined) return false;
          const barWidth = xScale.bandwidth();
          return barX + barWidth > x0 && barX < x1;
        });

        console.log(`[${this.container.id}] Selected ${selectedData.length} bars out of ${this.chartData.length} total bars.`);

        // Apply visual selection - dim unselected bars
        const selectedSet = new Set(selectedData);
        this.g.selectAll('.bar').style('opacity', (d: any) => {
          return selectedSet.has(d) ? 1.0 : 0.2;
        });

        // Apply filter through FilterManager
        if (this.filterManager && selectedData.length > 0) {
          const values = selectedData.map((d: any) => d[this.xColumnName]);
          console.log(`[${this.container.id}] Applying bar chart filter with values:`, values);
          this.filterManager.setFilters(
            { [this.xColumnName]: { type: "IN", values: new Set(values) } },
            this // Pass chart instance as source
          );
        }
      });

    this.g.append('g')
      .attr('class', 'brush')
      .call(brush);

    // Create bars
    const bars = this.g.selectAll('.bar')
      .data(this.chartData)
      .enter().append('rect')
      .attr('class', 'bar')
      .attr('x', (d: any) => xScale(d[this.xColumnName]))
      .attr('y', (d: any) => yScale(d.__aggregated_value))
      .attr('width', xScale.bandwidth())
      .attr('height', (d: any) => this.height - this.margin.top - this.margin.bottom - yScale(d.__aggregated_value))
      .attr('fill', 'steelblue');

    this.g.append('g')
      .attr('class', 'grid')
      .call(d3.axisLeft(yScale).tickSize(-(this.width - this.margin.left - this.margin.right)).tickFormat('' as any))
      .call((g: any) => g.select('.domain').remove())
      .selectAll('line')
      .attr('stroke', 'rgba(0,0,0,0.1)');


    const tooltipManager = TooltipManager.getInstance();
    tooltipManager.addTooltip(bars, (d: any) => {
      return `
        <strong>${this.xColumnName}:</strong> ${d[this.xColumnName]}<br>
        <strong>${this.yColumnName}:</strong> ${d.__aggregated_value}
      `;
    });
  }

  async drawChart() {
    await this.render();
  }

  redrawWithoutRefetch() {
    this.render();
  }

  destroy() {
    if (this.resizeTimeout) {
      clearTimeout(this.resizeTimeout);
    }
  }
}

// --- Mithril component (unchanged externally) ---
export class D3BarChartComponent implements m.Component<{
  dataProvider: any,
  xColumnName: string,
  yColumnName: string,
  aggregationFunction: string,
  filterManager?: any // Add this
}> {
  chart: D3BarChart|null = null;
  dom: HTMLElement | null = null;

  oncreate(vnode: m.VnodeDOM<{
    dataProvider: any,
    xColumnName: string,
    yColumnName: string,
    aggregationFunction: string,
    filterManager?: any
  }>) {
    this.dom = vnode.dom as HTMLElement;
    this.dom.id = `d3-bar-chart-${Math.random().toString(36).substr(2, 9)}`;

    this.chart = new D3BarChart(this.dom, {
      ...vnode.attrs,
      filterManager: vnode.attrs.filterManager // Pass FilterManager to D3 class
    });

    // Subscribe to filter updates
    if (vnode.attrs.filterManager) {
      vnode.attrs.filterManager.subscribe(this);
    }

    this.chart.render();
    ComponentManager.register(this);
    ChartManager.register(this);
  }

  // Required method for FilterManager integration
  async refreshFilteredData() {
    console.log(`[${this.chart?.container.id}] Bar chart refreshFilteredData called`);
    if (this.chart) {
      await this.chart.drawChart(); // Redraw with filtered data
      m.redraw(); // Trigger Mithril redraw
    }
  }

  // Required method for FilterManager integration
  setIsFilterSource(isSource: boolean) {
    console.log(`[${this.chart?.container.id}] Bar chart setIsFilterSource called with:`, isSource);
    if (this.chart) {
      this.chart.isFilterSource = isSource;
      // Update visual styling to indicate filter source
      if (this.dom) {
        const header = this.dom.querySelector('.chart-header');
        if (header) {
          header.classList.toggle('filter-source', isSource);
        }
      }

      // Auto-refresh when becoming filter source if flag is enabled
      if (isSource && (window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER === true) {
        this.refreshFilteredData();
      }
    }
  }

  onremove(vnode: any) {
    if (vnode.attrs.filterManager) {
      vnode.attrs.filterManager.unsubscribe(this);
      vnode.attrs.filterManager.clearFiltersForChart(this).then(() => {
        vnode.attrs.filterManager.notifySubscribers();
      });
    }

    if (this.chart) {
      this.chart.destroy();
    }
    ChartManager.unregister(this);
  }

  view() {
    return m(
        '.chart-container',
        {style: {width: '500px', height: '400px'}},
        m('.chart-header',
          {
            onclick: (event: MouseEvent) => {
              event.stopPropagation();
              if (this.chart?.filterManager) {
                this.chart.filterManager.clearFiltersForChart(this.chart);
              }
            },
          },
          m('h4.chart-title', 'Bar Chart'),
          m('.chart-actions',
            m('button.chart-action-btn',
              {title: 'Duplicate chart', onclick: () => ChartManager.duplicateChart(this.dom)},
              '⧉'),
            m('button.chart-close-btn',
              {title: 'Remove Chart', onclick: () => ChartManager.removeChart(this.dom)},
              '×'),
            ),
          ),
        m('.chart-content', {style: {'overflow-x': 'auto', 'overflow-y': 'hidden'}}),
        m('.resize-handle', {
          onmousedown: (e: MouseEvent) => {
            if (this.dom) {
              ResizeManager.getInstance().onMouseDown(
                  e, this.dom, this.dom);
            }
          },
        }),
    );
  }
}

class D3HistogramChart {
    container: HTMLElement;
    width: number = 600;
    height: number = 400;
    margin = { top: 10, right: 30, bottom: 150, left: 60 };
    svg: any;
    g: any;
    chartData: any[] = [];
    columnName: string;
    dataProvider: any;
    numBins: number = 20;
    filterManager: any;
    isFilterSource: boolean = false;

    constructor(container: HTMLElement, attrs: any) {
        this.container = container;
        this.dataProvider = attrs.dataProvider;
        this.columnName = attrs.columnName;
        this.filterManager = attrs.filterManager;
    }

    getContainerDimensions() {
        const rect = this.container.getBoundingClientRect();
        return {
            width: rect.width || 600,
            height: rect.height || 400,
        };
    }

    async loadData() {
        console.log(`[${this.container.id}] Loading data for histogram...`);
        console.log(`[${this.container.id}] FilterManager active filters:`, this.filterManager ? this.filterManager.getFilters() : 'No FilterManager');

        const querySpec = {
          filters: this.filterManager ? this.filterManager.getFiltersForQuery() : [],
        };
        const result = await this.dataProvider.query(querySpec);
        this.chartData = result.data;

        console.log(`[${this.container.id}] Data loaded for histogram:`, this.chartData);
        console.log(`[${this.container.id}] Raw histogram data length:`, this.chartData.length);
    }

    setupSvgWithDimensions(chartContent: any) {
        if (!chartContent) return null;
        d3.select(chartContent).selectAll("*").remove();
        const containerDimensions = this.getContainerDimensions();
        this.width = containerDimensions.width;
        this.height = containerDimensions.height;
        this.svg = d3.select(chartContent)
            .append('svg')
            .attr('width', this.width)
            .attr('height', this.height);
        this.g = this.svg.append('g').attr('transform', `translate(${this.margin.left},${this.margin.top})`);
        return this.svg;
    }

    async render() {
        console.log(`[${this.container.id}] Rendering histogram...`);
        await this.loadData();
        const chartContent = this.container.querySelector('.chart-content');
        if (!chartContent) {
            console.error(`[${this.container.id}] Histogram content area not found.`);
            return;
        }
        this.setupSvgWithDimensions(chartContent);

        // Handle empty data case
        if (!this.chartData || this.chartData.length === 0) {
            console.log(`[${this.container.id}] No data to render histogram, skipping chart rendering`);
            // Add a "No data" message
            this.g.append('text')
                .attr('x', (this.width - this.margin.left - this.margin.right) / 2)
                .attr('y', (this.height - this.margin.top - this.margin.bottom) / 2)
                .attr('text-anchor', 'middle')
                .style('font-size', '14px')
                .style('fill', '#666')
                .text('No data available');
            return;
        }

        const values = this.chartData.map(d => +d[this.columnName]).filter(v => !isNaN(v));

        // Handle case where all values are filtered out
        if (values.length === 0) {
            console.log(`[${this.container.id}] No valid numeric values for histogram`);
            this.g.append('text')
                .attr('x', (this.width - this.margin.left - this.margin.right) / 2)
                .attr('y', (this.height - this.margin.top - this.margin.bottom) / 2)
                .attr('text-anchor', 'middle')
                .style('font-size', '14px')
                .style('fill', '#666')
                .text('No valid data for histogram');
            return;
        }

        const xScale = d3.scaleLinear()
            .domain(d3.extent(values) as [number, number])
            .range([0, this.width - this.margin.left - this.margin.right]);

        const histogram = d3.histogram()
            .value((d: any) => d)
            .domain(xScale.domain() as [number, number])
            .thresholds(xScale.ticks(this.numBins));

        const bins = histogram(values);

        const yScale = d3.scaleLinear()
            .domain([0, d3.max(bins, (d: any) => d.length) as number])
            .nice()
            .range([this.height - this.margin.top - this.margin.bottom, 0]);

        const brush = d3.brushX()
            .extent([[0, 0], [this.width - this.margin.left - this.margin.right, this.height - this.margin.top - this.margin.bottom]])
            .on('end', (event: { selection: any; }) => {
                console.log(`[${this.container.id}] Histogram brush event ended.`);
                if (!event.selection) {
                    console.log(`[${this.container.id}] No selection, clearing filters and resetting opacity.`);
                    // Reset all bars to full opacity when no selection
                    this.g.selectAll('.bar').style('opacity', 1.0);
                    if (this.filterManager) {
                        console.log(`[${this.container.id}] Clearing filters for this chart.`);
                        this.filterManager.clearFiltersForChart(this);
                    }
                    return;
                }
                const [x0, x1] = event.selection;
                const min = xScale.invert(x0);
                const max = xScale.invert(x1);
                console.log(`[${this.container.id}] Brush selection inverted to data range: [${min}, ${max}]`);

                // Apply visual selection - dim bars outside the selected range
                this.g.selectAll('.bar').style('opacity', (d: any) => {
                    const barMin = d.x0;
                    const barMax = d.x1;
                    // Check if bar overlaps with selection
                    const isSelected = barMax > min && barMin < max;
                    return isSelected ? 1.0 : 0.2;
                });

                console.log(`[${this.container.id}] Applied visual selection to histogram bars.`);

                if (this.filterManager) {
                    console.log(`[${this.container.id}] FilterManager found, setting filters.`);
                    console.log(`[${this.container.id}] Current data length before filter:`, this.chartData.length);

                    const newFilter = {
                        type: 'AND',
                        conditions: [
                            { type: 'RANGE', operator: '>=', value: min },
                            { type: 'RANGE', operator: '<=', value: max }
                        ],
                        raw: `>= ${min.toFixed(2)} AND <= ${max.toFixed(2)}`
                    };

                    console.log(`[${this.container.id}] Setting filter:`, newFilter);
                    // Pass the filter correctly
                    this.filterManager.setFilters({ [this.columnName]: newFilter }, this);
                } else {
                    console.log(`[${this.container.id}] No FilterManager found.`);
                }
            });

        this.g.append('g')
            .attr('class', 'brush')
            .call(brush);

        const bars = this.g.selectAll('.bar')
            .data(bins)
            .enter().append('rect')
            .attr('class', 'bar')
            .attr('x', (d: any) => xScale(d.x0!))
            .attr('y', (d: any) => yScale(d.length))
            .attr('width', (d: any) => Math.max(0, xScale(d.x1!) - xScale(d.x0!) - 1))
            .attr('height', (d: any) => this.height - this.margin.top - this.margin.bottom - yScale(d.length))
            .attr('fill', 'steelblue');

        const tooltipManager = TooltipManager.getInstance();
        tooltipManager.addTooltip(bars, (d: any) => {
          return `
            <strong>Range:</strong> [${d.x0.toFixed(2)}, ${d.x1.toFixed(2)})<br>
            <strong>Count:</strong> ${d.length}
          `;
        });

        this.g.append('g')
            .attr('transform', `translate(0,${this.height - this.margin.top - this.margin.bottom})`)
            .call(d3.axisBottom(xScale))
            .selectAll('text')
            .style('text-anchor', 'end')
            .attr('dx', '-.8em')
            .attr('dy', '.15em')
            .attr('transform', 'rotate(-45)')
            .text((d: any, i: number) => {
              if (i % 2 !== 0) return '';
              return truncate(String(d), 10);
            });

        this.g.append('g')
            .call(d3.axisLeft(yScale).tickFormat((d: any) => truncate(String(d), 10)));

        this.g.append('g')
            .attr('class', 'grid')
            .call(d3.axisLeft(yScale).tickSize(-(this.width - this.margin.left - this.margin.right)).tickFormat('' as any))
            .call((g: any) => g.select('.domain').remove())
            .selectAll('line')
            .attr('stroke', 'rgba(0,0,0,0.1)');
    }

    redrawWithoutRefetch() {
        this.render();
    }

    destroy() {}
}

export class D3HistogramComponent implements m.Component<{
    dataProvider: any,
    columnName: string,
    filterManager?: any,
}> {
    chart: D3HistogramChart|null = null;
    dom: HTMLElement | null = null;
    private initialized: boolean = false;

    oncreate(vnode: m.VnodeDOM<{
           dataProvider: any,
           columnName: string,
           filterManager?: any,
       }>) {
           console.log('D3HistogramComponent created with vnode.attrs:', vnode.attrs);
           this.dom = vnode.dom as HTMLElement;
           this.dom.id = `d3-histogram-chart-${Math.random().toString(36).substr(2, 9)}`;

           this.chart = new D3HistogramChart(this.dom, {
               ...vnode.attrs,
               filterManager: vnode.attrs.filterManager
           });

           // Subscribe to filter updates AFTER chart is created but BEFORE initial render
           if (vnode.attrs.filterManager) {
               vnode.attrs.filterManager.subscribe(this);
           }

           // Initial render
           this.chart.render().then(() => {
               this.initialized = true;
               ComponentManager.register(this);
               ChartManager.register(this);
           });
       }

    async refreshFilteredData() {
        console.log(`[${this.chart?.container.id}] Histogram refreshFilteredData called, initialized:`, this.initialized);
        // Only refresh if the chart has been fully initialized
        if (this.chart && this.initialized) {
            await this.chart.render();
            m.redraw();
        }
    }

    // Required method for FilterManager integration
    setIsFilterSource(isSource: boolean) {
        console.log(`[${this.chart?.container.id}] Histogram setIsFilterSource called with:`, isSource);
        if (this.chart) {
            this.chart.isFilterSource = isSource;
            // Update visual styling to indicate filter source
            if (this.dom) {
                const header = this.dom.querySelector('.chart-header');
                if (header) {
                    header.classList.toggle('filter-source', isSource);
                }
            }

            // Auto-refresh when becoming filter source if flag is enabled
            if (isSource && (window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER === true) {
                this.refreshFilteredData();
            }
        }
    }


    onremove(vnode: any) {
        if (vnode.attrs.filterManager) {
            vnode.attrs.filterManager.unsubscribe(this);
        }
        if (this.chart) {
          this.chart.destroy();
        }
        ChartManager.unregister(this);
      }

    view() {
        return m(
            '.chart-container',
            {style: {width: '500px', height: '400px'}},
            m('.chart-header',
              {
                onclick: (event: MouseEvent) => {
                  event.stopPropagation();
                  if (this.chart?.filterManager) {
                    this.chart.filterManager.clearFiltersForChart(this.chart);
                  }
                },
              },
              m('h4.chart-title', 'Histogram'),
              m('.chart-actions',
                m('button.chart-action-btn', {title: 'Duplicate chart', onclick: () => ChartManager.duplicateChart(this.dom)}, '⧉'),
                m('button.chart-close-btn', {title: 'Remove Chart', onclick: () => ChartManager.removeChart(this.dom)}, '×'),
              ),
            ),
            m('.chart-content'),
            m('.resize-handle', {
              onmousedown: (e: MouseEvent) => {
                if (this.dom) {
                  ResizeManager.getInstance().onMouseDown(
                      e, this.dom, this.dom);
                }
              },
            }),
        );
      }
}

class ClipPathManager {
  private svg: any;
  private defs: any;
  private clipPathCounter: number;
  private activeClips: Set<string>;

  constructor(svg: any) {
    this.svg = svg;
    this.defs = this.ensureDefs();
    this.clipPathCounter = 0;
    this.activeClips = new Set();
  }

  ensureDefs(): any {
    let defs = this.svg.select("defs");
    if (defs.empty()) {
      defs = this.svg.append("defs");
    }
    return defs;
  }

  createRectClip(x: number, y: number, width: number, height: number): string {
    const id = `clip-${this.clipPathCounter++}`;
    this.activeClips.add(id);

    const clipPath = this.defs.append("clipPath").attr("id", id);

    clipPath
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

class D3CDFChart {
        container: HTMLElement;
        width: number = 600;
        height: number = 400;
        margin = { top: 10, right: 180, bottom: 150, left: 60 };
        svg: any;
        g: any;
        chartData: any[] = [];
        cdfData: any[] = [];
        columnName: string;
        dataProvider: any;
        colorBy: string | null;
        line: any;
        xScale: any;
        yScale: any;
        colorScale: any;
        filterManager: any;
        isFilterSource: boolean = false;

        constructor(container: HTMLElement, attrs: any) {
            this.container = container;
            this.dataProvider = attrs.dataProvider;
            this.columnName = attrs.columnName;
            this.colorBy = attrs.colorBy || null;
            this.filterManager = attrs.filterManager;
        }

        getContainerDimensions() {
            const rect = this.container.getBoundingClientRect();
            return {
                width: rect.width || 600,
                height: rect.height || 400,
            };
        }

        async loadData() {
            console.log(`[${this.container.id}] Loading data for CDF...`);
            console.log(`[${this.container.id}] FilterManager active filters:`, this.filterManager ? this.filterManager.getFilters() : 'No FilterManager');

            const querySpec = {
              filters: this.filterManager ? this.filterManager.getFiltersForQuery() : [],
            };
            const result = await this.dataProvider.query(querySpec);
            this.chartData = result.data;

            console.log(`[${this.container.id}] Data loaded for CDF:`, this.chartData.length, 'rows');
        }

        setupSvgWithDimensions(chartContent: any) {
            if (!chartContent) return null;
            d3.select(chartContent).selectAll("*").remove();
            const containerDimensions = this.getContainerDimensions();
            this.width = containerDimensions.width;
            this.height = containerDimensions.height;
            this.svg = d3.select(chartContent)
                .append('svg')
                .attr('width', this.width)
                .attr('height', this.height);
            this.g = this.svg.append('g').attr('transform', `translate(${this.margin.left},${this.margin.top})`);
            return this.svg;
        }

      async render() {
            console.log(`[${this.container.id}] Rendering CDF...`);
            await this.loadData();
            const chartContent = this.container.querySelector('.chart-content');
            if (!chartContent) {
                console.error(`[${this.container.id}] CDF content area not found.`);
                return;
            }
            this.setupSvgWithDimensions(chartContent);

            // Handle empty data case
            if (!this.chartData || this.chartData.length === 0) {
                console.log(`[${this.container.id}] No data to render CDF, skipping chart rendering`);
                this.g.append('text')
                    .attr('x', (this.width - this.margin.left - this.margin.right) / 2)
                    .attr('y', (this.height - this.margin.top - this.margin.bottom) / 2)
                    .attr('text-anchor', 'middle')
                    .style('font-size', '14px')
                    .style('fill', '#666')
                    .text('No data available');
                return;
            }

            const allValues = this.chartData.map(d => +d[this.columnName]).filter(v => !isNaN(v));

            // Handle case where all values are filtered out
            if (allValues.length === 0) {
                console.log(`[${this.container.id}] No valid numeric values for CDF`);
                this.g.append('text')
                    .attr('x', (this.width - this.margin.left - this.margin.right) / 2)
                    .attr('y', (this.height - this.margin.top - this.margin.bottom) / 2)
                    .attr('text-anchor', 'middle')
                    .style('font-size', '14px')
                    .style('fill', '#666')
                    .text('No valid data for CDF');
                return;
            }

            this.xScale = d3.scaleLinear()
                .domain(d3.extent(allValues) as [number, number])
                .range([0, this.width - this.margin.left - this.margin.right]);

            this.yScale = d3.scaleLinear()
                .domain([0, 1])
                .range([this.height - this.margin.top - this.margin.bottom, 0]);

            this.line = d3.line()
                .x((d: any) => this.xScale(d.value))
                .y((d: any) => this.yScale(d.probability))
                .curve(d3.curveStepAfter);

            this.cdfData = [];
            if (this.colorBy) {
                this.colorScale = d3.scaleOrdinal(d3.schemeCategory10);
                const groupedData = d3.group(this.chartData, (d: any) => d[this.colorBy!]);
                for (const [key, groupData] of groupedData.entries()) {
                    const values = groupData.map((d: any) => +d[this.columnName]).filter((v: any) => !isNaN(v)).sort(d3.ascending);
                    const groupCdfData = values.map((value: any, i: any) => ({
                        value: value,
                        probability: (i + 1) / values.length,
                        group: key,
                    }));
                    this.cdfData.push(groupCdfData);
                    this.g.append('path')
                        .datum(groupCdfData)
                        .attr('class', 'cdf-line')
                        .attr('fill', 'none')
                        .attr('stroke', this.colorScale(key))
                        .attr('stroke-width', 1.5)
                        .attr('d', this.line);
                }
            } else {
                this.colorScale = d3.scaleOrdinal().range(['steelblue']);
                const values = allValues.sort(d3.ascending);
                const singleCdfData = values.map((value, i) => ({
                    value: value,
                    probability: (i + 1) / values.length,
                    group: 'default',
                }));
                this.cdfData.push(singleCdfData);
                this.g.append('path')
                    .datum(singleCdfData)
                    .attr('class', 'cdf-line')
                    .attr('fill', 'none')
                    .attr('stroke', 'steelblue')
                    .attr('stroke-width', 1.5)
                    .attr('d', this.line);
            }

            const tooltipManager = TooltipManager.getInstance();
            const focus = this.g.append('g')
                .attr('class', 'focus')
                .style('display', 'none');

            focus.append('circle')
                .attr('r', 5);

            this.g.append('rect')
                .attr('class', 'overlay')
                .attr('width', this.width - this.margin.left - this.margin.right)
                .attr('height', this.height - this.margin.top - this.margin.bottom)
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
                        focus.attr('transform', `translate(${this.xScale(closestPoint.value)},${this.yScale(closestPoint.probability)})`);
                        tooltipManager.show(`
                            <strong>Value:</strong> ${closestPoint.value}<br>
                            <strong>Probability:</strong> ${closestPoint.probability.toFixed(2)}
                        `, event);
                    }
                });

            this.g.append('g')
                .attr('transform', `translate(0,${this.height - this.margin.top - this.margin.bottom})`)
                .call(d3.axisBottom(this.xScale))
                .selectAll('text')
                .style('text-anchor', 'end')
                .attr('dx', '-.8em')
                .attr('dy', '.15em')
                .attr('transform', 'rotate(-45)')
                .text((d: any, i: number) => {
                  if (i % 2 !== 0) return '';
                  return truncate(String(d), 10);
                });

            this.g.append('g')
                .call(d3.axisLeft(this.yScale).tickFormat((d: any) => truncate(String(d3.format('.0%')(d)), 10)));

            this.g.append('g')
                .attr('class', 'grid')
                .call(d3.axisLeft(this.yScale).tickSize(-(this.width - this.margin.left - this.margin.right)).tickFormat('' as any))
                .call((g: any) => g.select('.domain').remove())
                .selectAll('line')
                .attr('stroke', 'rgba(0,0,0,0.1)');

            const brush = d3.brushX()
                .extent([[0, 0], [this.width - this.margin.left - this.margin.right, this.height - this.margin.top - this.margin.bottom]])
                .on('end', (event: { selection: any; }) => {
                    console.log(`[${this.container.id}] CDF brush event ended.`);
                    const { selection } = event;
                    if (!selection) {
                        console.log(`[${this.container.id}] No selection, clearing filters.`);
                        this.highlightSelection([]);
                        if (this.filterManager) {
                            this.filterManager.clearFiltersForChart(this);
                        }
                        return;
                    }
                    const selectedData = this.getBrushedItems(selection);
                    this.highlightSelection(selectedData);

                    // Apply filter through FilterManager if we have selected data
                    if (this.filterManager && selectedData.length > 0) {
                        console.log(`[${this.container.id}] Applying CDF filter with ${selectedData.length} selected points`);

                        // Get the range of selected values
                        const values = selectedData.map(d => d.value);
                        const valueRange = d3.extent(values) as [number, number];

                        const filter = {
                            type: 'AND',
                            conditions: [
                                { type: 'RANGE', operator: '>=', value: valueRange[0] },
                                { type: 'RANGE', operator: '<=', value: valueRange[1] }
                            ],
                            raw: `${this.columnName}: ${valueRange[0].toFixed(2)} - ${valueRange[1].toFixed(2)}`
                        };

                        this.filterManager.setFilters({ [this.columnName]: filter }, this);
                    }
                });

            this.g.append('g')
                .attr('class', 'brush')
                .call(brush);
    
            if (this.colorBy) {
                this.renderLegend();
            }
        }

        getBrushedItems(selection: any): any[] {
            const [x0, x1] = selection;
            const minValue = this.xScale.invert(x0);
            const maxValue = this.xScale.invert(x1);

            const flatData = this.cdfData.flat().sort((a:any, b:any) => a.value - b.value);
            const selectedData: any[] = [];

            for (let i = 0; i < flatData.length; i++) {
                const currentPoint = flatData[i];
                const nextPoint = flatData[i + 1];

                const currentX = currentPoint.value;
                const nextX = nextPoint ? nextPoint.value : currentX;

                // A horizontal segment exists from currentX to nextX
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

            const totalPoints = this.cdfData.reduce(
              (sum: any, group: any) => sum + group.length,
              0,
            );
            const hasSelection =
              selectedData.length > 0 && selectedData.length < totalPoints;

            this.g.selectAll('.cdf-line').style('opacity', hasSelection ? 0.2 : 1.0);

            if (hasSelection) {
              const selectedByGroup = d3.group(selectedData, (d: any) => d.group);
              for (const [group, groupSelectedData] of selectedByGroup) {
                this.g
                  .append('path')
                  .datum(groupSelectedData.sort((a: any, b: any) => a.value - b.value))
                  .attr('class', 'cdf-line-highlight')
                  .attr('d', this.line)
                  .attr('fill', 'none')
                  .attr('stroke', this.colorScale(group))
                  .style('stroke-width', 1.5);
              }
            }
        }

        redrawWithoutRefetch() {
            this.render();
        }

        destroy() {}
    
        renderLegend() {
            const chartContent = this.container.querySelector('.chart-content');
            if (!chartContent) return;
            d3.select(chartContent).select('.chart-legend').remove();
    
            const legendContainer = d3.select(chartContent).append('div').attr('class', 'chart-legend');
    
            const maxLegendItems = 10;
            const legendData = this.colorScale.domain();
            const truncated = legendData.length > maxLegendItems;
            const data = truncated ? legendData.slice(0, maxLegendItems) : legendData;

            const legendItems = legendContainer.selectAll('.legend-item')
                .data(data)
                .enter()
                .append('div')
                .attr('class', 'legend-item')
                .on('click', (_event: MouseEvent, d: any) => {
                    if (this.filterManager && this.colorBy) {
                        this.filterManager.setFilters(
                            {[this.colorBy]: {type: "IN", values: new Set([d])}},
                            this
                        );
                    }
                });
    
            legendItems.append('span')
                .attr('class', 'legend-swatch')
                .style('background-color', (d: any) => this.colorScale(d));
    
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

    export class D3CDFComponent implements m.Component<{
        dataProvider: any,
        columnName: string,
        colorBy?: string,
        filterManager?: any,
    }> {
        chart: D3CDFChart|null = null;
        dom: HTMLElement | null = null;

        oncreate(vnode: m.VnodeDOM<{
            dataProvider: any,
            columnName: string,
            colorBy?: string,
            filterManager?: any,
        }>) {
            this.dom = vnode.dom as HTMLElement;
            this.dom.id = `d3-cdf-chart-${Math.random().toString(36).substr(2, 9)}`;

            this.chart = new D3CDFChart(this.dom, vnode.attrs);
            if (vnode.attrs.filterManager) {
                vnode.attrs.filterManager.subscribe(this);
            }
            this.chart.render();
            ComponentManager.register(this);
            ChartManager.register(this);
        }

        async refreshFilteredData() {
            console.log(`[${this.chart?.container.id}] CDF refreshFilteredData called`);
            if (this.chart) {
                await this.chart.render();
                m.redraw();
            }
        }

        // Required method for FilterManager integration
        setIsFilterSource(isSource: boolean) {
            console.log(`[${this.chart?.container.id}] CDF setIsFilterSource called with:`, isSource);
            if (this.chart) {
                this.chart.isFilterSource = isSource;
                // Update visual styling to indicate filter source
                if (this.dom) {
                    const header = this.dom.querySelector('.chart-header');
                    if (header) {
                        header.classList.toggle('filter-source', isSource);
                    }
                }

                // Auto-refresh when becoming filter source if flag is enabled
                if (isSource && (window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER === true) {
                    this.refreshFilteredData();
                }
            }
        }

        onremove(vnode: any) {
            if (vnode.attrs.filterManager) {
                vnode.attrs.filterManager.unsubscribe(this);
            }
            if (this.chart) {
              this.chart.destroy();
            }
            ChartManager.unregister(this);
          }

        view() {
            return m(
                '.chart-container',
                {style: {width: '500px', height: '400px'}},
                m('.chart-header',
                  {
                    onclick: (event: MouseEvent) => {
                      event.stopPropagation();
                      if (this.chart?.filterManager) {
                        this.chart.filterManager.clearFiltersForChart(this.chart);
                      }
                    },
                  },
                  m('h4.chart-title', 'CDF'),
                  m('.chart-actions',
                    m('button.chart-action-btn', {title: 'Duplicate chart', onclick: () => ChartManager.duplicateChart(this.dom)}, '⧉'),
                    m('button.chart-close-btn', {title: 'Remove Chart', onclick: () => ChartManager.removeChart(this.dom)}, '×'),
                  ),
                ),
                m('.chart-content'),
                m('.resize-handle', {
                  onmousedown: (e: MouseEvent) => {
                    if (this.dom) {
                      ResizeManager.getInstance().onMouseDown(
                          e, this.dom, this.dom);
                    }
                  },
                }),
            );
        }
    }

class D3ScatterChart {
    container: HTMLElement;
    width: number = 600;
    height: number = 400;
    margin = { top: 10, right: 180, bottom: 150, left: 60 };
    svg: any;
    g: any;
    chartData: any[] = [];
    xColumnName: string;
    yColumnName: string;
    colorBy: string | null;
    dataProvider: any;
    xScale: any;
    yScale: any;
    colorScale: any;
    filterManager: any;
    isFilterSource: boolean = false;

    constructor(container: HTMLElement, attrs: any) {
        this.container = container;
        this.dataProvider = attrs.dataProvider;
        this.xColumnName = attrs.xColumnName;
        this.yColumnName = attrs.yColumnName;
        this.colorBy = attrs.colorBy || null;
        this.filterManager = attrs.filterManager;
    }

    getContainerDimensions() {
        const rect = this.container.getBoundingClientRect();
        return {
            width: rect.width || 600,
            height: rect.height || 400,
        };
    }

    async loadData() {
        console.log(`[${this.container.id}] Loading data for scatter plot...`);
        console.log(`[${this.container.id}] FilterManager active filters:`, this.filterManager ? this.filterManager.getFilters() : 'No FilterManager');

        const querySpec = {
          filters: this.filterManager ? this.filterManager.getFiltersForQuery() : [],
        };
        const result = await this.dataProvider.query(querySpec);
        this.chartData = result.data;

        console.log(`[${this.container.id}] Data loaded for scatter plot:`, this.chartData.length, 'rows');
    }

    setupSvgWithDimensions(chartContent: any) {
        if (!chartContent) return null;
        d3.select(chartContent).selectAll("*").remove();
        const containerDimensions = this.getContainerDimensions();
        this.width = containerDimensions.width;
        this.height = containerDimensions.height;
        this.svg = d3.select(chartContent)
            .append('svg')
            .attr('width', this.width)
            .attr('height', this.height);
        this.g = this.svg.append('g').attr('transform', `translate(${this.margin.left},${this.margin.top})`);
        return this.svg;
    }

    async render() {
        console.log(`[${this.container.id}] Rendering scatter plot...`);
        await this.loadData();
        const chartContent = this.container.querySelector('.chart-content');
        if (!chartContent) {
            console.error(`[${this.container.id}] Scatter plot content area not found.`);
            return;
        }
        this.setupSvgWithDimensions(chartContent);

        // Handle empty data case
        if (!this.chartData || this.chartData.length === 0) {
            console.log(`[${this.container.id}] No data to render scatter plot, skipping chart rendering`);
            this.g.append('text')
                .attr('x', (this.width - this.margin.left - this.margin.right) / 2)
                .attr('y', (this.height - this.margin.top - this.margin.bottom) / 2)
                .attr('text-anchor', 'middle')
                .style('font-size', '14px')
                .style('fill', '#666')
                .text('No data available');
            return;
        }

        const chartWidth = this.width - this.margin.left - this.margin.right;
        const chartHeight = this.height - this.margin.top - this.margin.bottom;

        const xData = this.chartData.map((d: any) => +d[this.xColumnName]).filter(v => !isNaN(v));
        const yData = this.chartData.map((d: any) => +d[this.yColumnName]).filter(v => !isNaN(v));

        // Handle case where all values are filtered out
        if (xData.length === 0 || yData.length === 0) {
            console.log(`[${this.container.id}] No valid numeric values for scatter plot`);
            this.g.append('text')
                .attr('x', chartWidth / 2)
                .attr('y', chartHeight / 2)
                .attr('text-anchor', 'middle')
                .style('font-size', '14px')
                .style('fill', '#666')
                .text('No valid data for scatter plot');
            return;
        }

        this.xScale = d3.scaleLinear()
            .domain(d3.extent(xData) as [number, number])
            .range([0, chartWidth])
            .nice();

        this.yScale = d3.scaleLinear()
            .domain(d3.extent(yData) as [number, number])
            .range([chartHeight, 0])
            .nice();

        if (this.colorBy) {
            const colorValues = [...new Set(this.chartData.map((d: any) => d[this.colorBy!]))];
            this.colorScale = d3.scaleOrdinal(d3.schemeCategory10).domain(colorValues);
        }

        const brush = d3.brush()
            .extent([[0, 0], [chartWidth, chartHeight]])
            .on('end', (event: { selection: any; }) => {
                console.log(`[${this.container.id}] Scatter brush event ended.`);
                if (!event.selection) {
                    console.log(`[${this.container.id}] No selection, clearing filters.`);
                    this.g.selectAll('.dot').style('opacity', 1.0);
                    if (this.filterManager) {
                        this.filterManager.clearFiltersForChart(this);
                    }
                    return;
                }
                const [[x0, y0], [x1, y1]] = event.selection;
                const selectedData = this.chartData.filter((d: any) => {
                    const x = this.xScale(+d[this.xColumnName]);
                    const y = this.yScale(+d[this.yColumnName]);
                    return x0 <= x && x <= x1 && y0 <= y && y <= y1;
                });

                // Apply visual selection
                const selectedSet = new Set(selectedData);
                this.g.selectAll('.dot').style('opacity', (d: any) => {
                    return selectedSet.has(d) ? 1.0 : 0.2;
                });

                // Apply filter through FilterManager if we have selected data
                if (this.filterManager && selectedData.length > 0) {
                    console.log(`[${this.container.id}] Applying scatter plot filter with ${selectedData.length} selected points`);

                    // Get the range of selected values for both axes
                    const xValues = selectedData.map(d => +d[this.xColumnName]);
                    const yValues = selectedData.map(d => +d[this.yColumnName]);
                    const xRange = d3.extent(xValues) as [number, number];
                    const yRange = d3.extent(yValues) as [number, number];

                    const filters: any = {};

                    // Add X-axis filter
                    filters[this.xColumnName] = {
                        type: 'AND',
                        conditions: [
                            { type: 'RANGE', operator: '>=', value: xRange[0] },
                            { type: 'RANGE', operator: '<=', value: xRange[1] }
                        ],
                        raw: `${this.xColumnName}: ${xRange[0].toFixed(2)} - ${xRange[1].toFixed(2)}`
                    };

                    // Add Y-axis filter
                    filters[this.yColumnName] = {
                        type: 'AND',
                        conditions: [
                            { type: 'RANGE', operator: '>=', value: yRange[0] },
                            { type: 'RANGE', operator: '<=', value: yRange[1] }
                        ],
                        raw: `${this.yColumnName}: ${yRange[0].toFixed(2)} - ${yRange[1].toFixed(2)}`
                    };

                    this.filterManager.setFilters(filters, this);
                }
            });

        this.g.append('g')
            .attr('class', 'brush')
            .call(brush);

        const dots = this.g.selectAll('.dot')
            .data(this.chartData)
            .enter().append('circle')
            .attr('class', 'dot')
            .attr('r', 4)
            .attr('cx', (d: any) => this.xScale(+d[this.xColumnName]))
            .attr('cy', (d: any) => this.yScale(+d[this.yColumnName]))
            .style('fill', (d: any) => this.colorBy ? this.colorScale(d[this.colorBy]) : 'steelblue');

        const tooltipManager = TooltipManager.getInstance();
        tooltipManager.addTooltip(dots, (d: any) => {
          let content = `
            <strong>${this.xColumnName}:</strong> ${d[this.xColumnName]}<br>
            <strong>${this.yColumnName}:</strong> ${d[this.yColumnName]}
          `;
          if (this.colorBy) {
            content += `<br><strong>${this.colorBy}:</strong> ${d[this.colorBy]}`;
          }
          return content;
        });

        this.g.append('g')
            .attr('transform', `translate(0,${chartHeight})`)
            .call(d3.axisBottom(this.xScale))
            .selectAll('text')
            .style('text-anchor', 'end')
            .attr('dx', '-.8em')
            .attr('dy', '.15em')
            .attr('transform', 'rotate(-45)')
            .text((d: any, i: number) => {
              if (i % 2 !== 0) return '';
              return truncate(String(d), 10);
            });

        this.g.append('g')
            .call(d3.axisLeft(this.yScale).tickFormat((d: any) => truncate(String(d), 10)));

        this.g.append('g')
            .attr('class', 'grid')
            .call(d3.axisLeft(this.yScale).tickSize(-chartWidth).tickFormat('' as any))
            .call((g: any) => g.select('.domain').remove())
            .selectAll('line')
            .attr('stroke', 'rgba(0,0,0,0.1)');

        this.g.append('g')
            .attr('class', 'grid')
            .call(d3.axisLeft(this.yScale).tickSize(-chartWidth).tickFormat('' as any))
            .call((g: any) => g.select('.domain').remove())
            .selectAll('line')
            .attr('stroke', 'rgba(0,0,0,0.1)');

        this.drawCorrelationLine();

        if (this.colorBy) {
            this.renderLegend();
        }
    }

    redrawWithoutRefetch() {
        this.render();
    }

    drawCorrelationLine() {
      if (!this.g || this.chartData.length < 2) return;

      // Remove existing correlation elements
      this.g.selectAll('.correlation-line').remove();
      this.g.selectAll('.correlation-text').remove();

      const { r, slope, intercept } = this.calculateCorrelation(
        this.chartData,
      );

      // Get the domain of x values for the line
      const xDomain = this.xScale.domain();
      const x1 = xDomain[0];
      const x2 = xDomain[1];
      const y1 = slope * x1 + intercept;
      const y2 = slope * x2 + intercept;

      // Draw correlation line
      this.g
        .append('line')
        .attr('class', 'correlation-line')
        .attr('x1', this.xScale(x1))
        .attr('y1', this.yScale(y1))
        .attr('x2', this.xScale(x2))
        .attr('y2', this.yScale(y2))
        .attr('stroke', '#666666')
        .attr('stroke-width', 1.5)
        .attr('stroke-dasharray', '5,5')
        .attr('opacity', 0.7);

      // Add correlation coefficient text
      const chartWidth = this.width - this.margin.left - this.margin.right;
      this.g
        .append('text')
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

      const numerator = d3.sum(data, (d: any) => (+d[this.xColumnName] - xMean) * (+d[this.yColumnName] - yMean));
      const xSumSquares = d3.sum(xValues, (x: any) => Math.pow(x - xMean, 2));
      const ySumSquares = d3.sum(yValues, (y: any) => Math.pow(y - yMean, 2));

      const denominator = Math.sqrt(xSumSquares * ySumSquares);
      const r = denominator === 0 ? 0 : numerator / denominator;

      const slope = denominator === 0 ? 0 : numerator / xSumSquares;
      const intercept = (yMean ?? 0) - slope * (xMean ?? 0);

      return { r, slope, intercept };
    }

    destroy() {}

    renderLegend() {
        const chartContent = this.container.querySelector('.chart-content');
        if (!chartContent) return;
        d3.select(chartContent).select('.chart-legend').remove();

        const legendContainer = d3.select(chartContent).append('div').attr('class', 'chart-legend');

        const maxLegendItems = 10;
        const legendData = this.colorScale.domain();
        const truncated = legendData.length > maxLegendItems;
        const data = truncated ? legendData.slice(0, maxLegendItems) : legendData;

        const legendItems = legendContainer.selectAll('.legend-item')
            .data(data)
            .enter()
            .append('div')
            .attr('class', 'legend-item')
            .on('click', (_event: MouseEvent, d: any) => {
                if (this.filterManager && this.colorBy) {
                    this.filterManager.setFilters(
                        {[this.colorBy]: {type: "IN", values: new Set([d])}},
                        this
                    );
                }
            });

        legendItems.append('span')
            .attr('class', 'legend-swatch')
            .style('background-color', (d: any) => this.colorScale(d));

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

export class D3ScatterChartComponent implements m.Component<{
    dataProvider: any,
    xColumnName: string,
    yColumnName: string,
    colorBy?: string,
    filterManager?: any,
}> {
    chart: D3ScatterChart|null = null;
    dom: HTMLElement | null = null;

    oncreate(vnode: m.VnodeDOM<{
        dataProvider: any,
        xColumnName: string,
        yColumnName: string,
        colorBy?: string,
        filterManager?: any,
    }>) {
        this.dom = vnode.dom as HTMLElement;
        this.dom.id = `d3-scatter-chart-${Math.random().toString(36).substr(2, 9)}`;

        this.chart = new D3ScatterChart(this.dom, vnode.attrs);
        if (vnode.attrs.filterManager) {
            vnode.attrs.filterManager.subscribe(this);
        }
        this.chart.render();
        ComponentManager.register(this);
        ChartManager.register(this);
    }

    async refreshFilteredData() {
        console.log(`[${this.chart?.container.id}] Scatter chart refreshFilteredData called`);
        if (this.chart) {
            await this.chart.render();
            m.redraw();
        }
    }

    // Required method for FilterManager integration
    setIsFilterSource(isSource: boolean) {
        console.log(`[${this.chart?.container.id}] Scatter chart setIsFilterSource called with:`, isSource);
        if (this.chart) {
            this.chart.isFilterSource = isSource;
            // Update visual styling to indicate filter source
            if (this.dom) {
                const header = this.dom.querySelector('.chart-header');
                if (header) {
                    header.classList.toggle('filter-source', isSource);
                }
            }

            // Auto-refresh when becoming filter source if flag is enabled
            if (isSource && (window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER === true) {
                this.refreshFilteredData();
            }
        }
    }

    onremove(vnode: any) {
        if (vnode.attrs.filterManager) {
            vnode.attrs.filterManager.unsubscribe(this);
        }
        if (this.chart) {
          this.chart.destroy();
        }
        ChartManager.unregister(this);
      }

    view() {
        return m(
            '.chart-container',
            {style: {width: '500px', height: '400px'}},
            m('.chart-header',
              {
                onclick: (event: MouseEvent) => {
                  event.stopPropagation();
                  if (this.chart?.filterManager) {
                    this.chart.filterManager.clearFiltersForChart(this.chart);
                  }
                },
              },
              m('h4.chart-title', 'Scatter Plot'),
              m('.chart-actions',
                m('button.chart-action-btn', {title: 'Duplicate chart', onclick: () => ChartManager.duplicateChart(this.dom)}, '⧉'),
                m('button.chart-close-btn', {title: 'Remove Chart', onclick: () => ChartManager.removeChart(this.dom)}, '×'),
              ),
            ),
            m('.chart-content'),
            m('.resize-handle', {
              onmousedown: (e: MouseEvent) => {
                if (this.dom) {
                  ResizeManager.getInstance().onMouseDown(
                      e, this.dom, this.dom);
                }
              },
            }),
        );
    }
}

class D3HeatmapChart {
    container: HTMLElement;
    width: number = 600;
    height: number = 400;
    margin = { top: 10, right: 30, bottom: 150, left: 60 };
    svg: any;
    g: any;
    chartData: any[] = [];
    xColumnName: string;
    yColumnName: string;
    valueColumnName: string;
    aggregationFunction: string;
    dataProvider: any;
    xScale: any;
    yScale: any;
    colorScale: any;
    filterManager: any;
    isFilterSource: boolean = false;

    constructor(container: HTMLElement, attrs: any) {
        this.container = container;
        this.dataProvider = attrs.dataProvider;
        this.xColumnName = attrs.xColumnName;
        this.yColumnName = attrs.yColumnName;
        this.valueColumnName = attrs.valueColumnName;
        this.aggregationFunction = attrs.aggregationFunction || 'sum';
        this.filterManager = attrs.filterManager;
    }

    getContainerDimensions() {
        const rect = this.container.getBoundingClientRect();
        return {
            width: rect.width || 600,
            height: rect.height || 400,
        };
    }

    async loadData() {
        console.log(`[${this.container.id}] Loading data for heatmap...`);
        console.log(`[${this.container.id}] FilterManager active filters:`, this.filterManager ? this.filterManager.getFilters() : 'No FilterManager');

        const querySpec = {
            aggregation: {
                groupBy: [this.xColumnName, this.yColumnName],
                field: this.valueColumnName,
                function: this.aggregationFunction
            }
        };
        const querySpecWithFilters = {
          ...querySpec,
          filters: this.filterManager ? this.filterManager.getFiltersForQuery() : [],
        };
        const result = await this.dataProvider.query(querySpecWithFilters);
        this.chartData = result.data;

        console.log(`[${this.container.id}] Data loaded for heatmap:`, this.chartData.length, 'rows');
    }

    setupSvgWithDimensions(chartContent: any) {
        if (!chartContent) return null;
        d3.select(chartContent).selectAll("*").remove();
        const containerDimensions = this.getContainerDimensions();
        this.width = containerDimensions.width;
        this.height = containerDimensions.height;
        this.svg = d3.select(chartContent)
            .append('svg')
            .attr('width', this.width)
            .attr('height', this.height);
        this.g = this.svg.append('g').attr('transform', `translate(${this.margin.left},${this.margin.top})`);
        return this.svg;
    }

    async render() {
        console.log(`[${this.container.id}] Rendering heatmap...`);
        await this.loadData();
        const chartContent = this.container.querySelector('.chart-content');
        if (!chartContent) {
            console.error(`[${this.container.id}] Heatmap content area not found.`);
            return;
        }
        this.setupSvgWithDimensions(chartContent);

        // Handle empty data case
        if (!this.chartData || this.chartData.length === 0) {
            console.log(`[${this.container.id}] No data to render heatmap, skipping chart rendering`);
            this.g.append('text')
                .attr('x', (this.width - this.margin.left - this.margin.right) / 2)
                .attr('y', (this.height - this.margin.top - this.margin.bottom) / 2)
                .attr('text-anchor', 'middle')
                .style('font-size', '14px')
                .style('fill', '#666')
                .text('No data available');
            return;
        }

        const chartWidth = this.width - this.margin.left - this.margin.right;
        const chartHeight = this.height - this.margin.top - this.margin.bottom;

        const xValues = [...new Set(this.chartData.map((d: any) => d[this.xColumnName]))];
        const yValues = [...new Set(this.chartData.map((d: any) => d[this.yColumnName]))];
        const valueExtent = d3.extent(this.chartData, (d: any) => d.__aggregated_value);

        this.xScale = d3.scaleBand()
            .domain(xValues)
            .range([0, chartWidth])
            .padding(0.05);

        this.yScale = d3.scaleBand()
            .domain(yValues)
            .range([chartHeight, 0])
            .padding(0.05);

        this.colorScale = d3.scaleSequential(d3.interpolateBlues)
            .domain(valueExtent as [number, number]);

        // Add brush first (following bar chart pattern)
        const brush = d3.brush()
            .extent([[0, 0], [chartWidth, chartHeight]])
            .on('end', (event: { selection: any; }) => {
                console.log(`[${this.container.id}] Heatmap brush event ended.`);
                if (!event.selection) {
                    console.log(`[${this.container.id}] No selection, clearing filters.`);
                    this.g.selectAll('.heatmap-cell').style('opacity', 1.0);
                    if (this.filterManager) {
                        this.filterManager.clearFiltersForChart(this);
                    }
                    return;
                }
                const [[x0, y0], [x1, y1]] = event.selection;
                const selectedData = this.chartData.filter((d: any) => {
                    const cellX = this.xScale(d[this.xColumnName]);
                    const cellY = this.yScale(d[this.yColumnName]);
                    const cellWidth = this.xScale.bandwidth();
                    const cellHeight = this.yScale.bandwidth();
                    return x0 < cellX + cellWidth && x1 > cellX && y0 < cellY + cellHeight && y1 > cellY;
                });

                // Apply visual selection
                const selectedSet = new Set(selectedData);
                this.g.selectAll('.heatmap-cell').style('opacity', (d: any) => {
                    return selectedSet.has(d) ? 1.0 : 0.2;
                });

                // Apply filter through FilterManager if we have selected data
                if (this.filterManager && selectedData.length > 0) {
                    console.log(`[${this.container.id}] Applying heatmap filter with ${selectedData.length} selected cells`);

                    // Get the unique values for both axes
                    const xValues = [...new Set(selectedData.map(d => d[this.xColumnName]))];
                    const yValues = [...new Set(selectedData.map(d => d[this.yColumnName]))];

                    const filters: any = {};

                    // Add X-axis filter
                    filters[this.xColumnName] = {
                        type: 'IN',
                        values: new Set(xValues),
                        raw: `${this.xColumnName} IN (${xValues.join(', ')})`
                    };

                    // Add Y-axis filter
                    filters[this.yColumnName] = {
                        type: 'IN',
                        values: new Set(yValues),
                        raw: `${this.yColumnName} IN (${yValues.join(', ')})`
                    };

                    this.filterManager.setFilters(filters, this);
                }
            });

        this.g.append('g')
            .attr('class', 'brush')
            .call(brush);

        // Create cells after brush (following bar chart pattern)
        const cells = this.g.selectAll('.heatmap-cell')
            .data(this.chartData, (d: any) => d[this.xColumnName] + ':' + d[this.yColumnName])
            .enter()
            .append('rect')
            .attr('class', 'heatmap-cell')
            .attr('x', (d: any) => this.xScale(d[this.xColumnName]))
            .attr('y', (d: any) => this.yScale(d[this.yColumnName]))
            .attr('width', this.xScale.bandwidth())
            .attr('height', this.yScale.bandwidth())
            .style('fill', (d: any) => this.colorScale(d.__aggregated_value));

        const tooltipManager = TooltipManager.getInstance();
        tooltipManager.addTooltip(cells, (d: any) => {
          return `
            <strong>${this.xColumnName}:</strong> ${d[this.xColumnName]}<br>
            <strong>${this.yColumnName}:</strong> ${d[this.yColumnName]}<br>
            <strong>${this.valueColumnName}:</strong> ${d.__aggregated_value}
          `;
        });

        this.g.append('g')
            .attr('transform', `translate(0,${chartHeight})`)
            .call(d3.axisBottom(this.xScale))
            .selectAll('text')
            .style('text-anchor', 'end')
            .attr('dx', '-.8em')
            .attr('dy', '.15em')
            .attr('transform', 'rotate(-45)')
            .text((d: any, i: number) => {
              if (i % 2 !== 0) return '';
              return truncate(String(d), 10);
            });

        this.g.append('g')
            .call(d3.axisLeft(this.yScale).tickFormat((d: any) => truncate(String(d), 10)));
    }

    redrawWithoutRefetch() {
        this.render();
    }

    destroy() {}
}

export class D3HeatmapChartComponent implements m.Component<{
    dataProvider: any,
    xColumnName: string,
    yColumnName: string,
    valueColumnName: string,
    filterManager?: any,
}> {
    chart: D3HeatmapChart|null = null;
    dom: HTMLElement | null = null;

    oncreate(vnode: m.VnodeDOM<{
        dataProvider: any,
        xColumnName: string,
        yColumnName: string,
        valueColumnName: string,
        filterManager?: any,
    }>) {
        this.dom = vnode.dom as HTMLElement;
        this.dom.id = `d3-heatmap-chart-${Math.random().toString(36).substr(2, 9)}`;

        this.chart = new D3HeatmapChart(this.dom, vnode.attrs);
        if (vnode.attrs.filterManager) {
            vnode.attrs.filterManager.subscribe(this);
        }
        this.chart.render();
        ComponentManager.register(this);
        ChartManager.register(this);
    }

    async refreshFilteredData() {
        console.log(`[${this.chart?.container.id}] Heatmap refreshFilteredData called`);
        if (this.chart) {
            await this.chart.render();
            m.redraw();
        }
    }

    // Required method for FilterManager integration
    setIsFilterSource(isSource: boolean) {
        console.log(`[${this.chart?.container.id}] Heatmap setIsFilterSource called with:`, isSource);
        if (this.chart) {
            this.chart.isFilterSource = isSource;
            // Update visual styling to indicate filter source
            if (this.dom) {
                const header = this.dom.querySelector('.chart-header');
                if (header) {
                    header.classList.toggle('filter-source', isSource);
                }
            }

            // Auto-refresh when becoming filter source if flag is enabled
            if (isSource && (window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER === true) {
                this.refreshFilteredData();
            }
        }
    }

    onremove(vnode: any) {
        if (vnode.attrs.filterManager) {
            vnode.attrs.filterManager.unsubscribe(this);
        }
        if (this.chart) {
          this.chart.destroy();
        }
        ChartManager.unregister(this);
      }

    view() {
        return m(
            '.chart-container',
            {style: {width: '500px', height: '400px'}},
            m('.chart-header',
              {
                onclick: (event: MouseEvent) => {
                  event.stopPropagation();
                  if (this.chart?.filterManager) {
                    this.chart.filterManager.clearFiltersForChart(this.chart);
                  }
                },
              },
              m('h4.chart-title', 'Heatmap'),
              m('.chart-actions',
                m('button.chart-action-btn', {title: 'Duplicate chart', onclick: () => ChartManager.duplicateChart(this.dom)}, '⧉'),
                m('button.chart-close-btn', {title: 'Remove Chart', onclick: () => ChartManager.removeChart(this.dom)}, '×'),
              ),
            ),
            m('.chart-content'),
            m('.resize-handle', {
              onmousedown: (e: MouseEvent) => {
                if (this.dom) {
                  ResizeManager.getInstance().onMouseDown(
                      e, this.dom, this.dom);
                }
              },
            }),
        );
    }
}

class D3BoxplotChart {
    container: HTMLElement;
    width: number = 600;
    height: number = 400;
    margin = { top: 10, right: 30, bottom: 150, left: 60 };
    svg: any;
    g: any;
    chartData: any[] = [];
    xColumnName: string;
    yColumnName: string;
    dataProvider: any;
    xScale: any;
    yScale: any;
    clipPathManager!: ClipPathManager;
    filterManager: any;
    isFilterSource: boolean = false;

    constructor(container: HTMLElement, attrs: any) {
        this.container = container;
        this.dataProvider = attrs.dataProvider;
        this.xColumnName = attrs.xColumnName;
        this.yColumnName = attrs.yColumnName;
        this.filterManager = attrs.filterManager;
    }

    getContainerDimensions() {
        const rect = this.container.getBoundingClientRect();
        return {
            width: rect.width || 600,
            height: rect.height || 400,
        };
    }

    async loadData() {
        console.log(`[${this.container.id}] Loading data for boxplot...`);
        console.log(`[${this.container.id}] FilterManager active filters:`, this.filterManager ? this.filterManager.getFilters() : 'No FilterManager');

        const querySpec = {
          filters: this.filterManager ? this.filterManager.getFiltersForQuery() : [],
        };
        const result = await this.dataProvider.query(querySpec);
        const data = result.data;

        const groupedData = d3.group(data, (d: any) => d[this.xColumnName]);
        this.chartData = Array.from(groupedData, ([key, values]) => {
            const sortedValues = values.map((d: any) => +d[this.yColumnName]).sort(d3.ascending);
            const q1 = d3.quantile(sortedValues, 0.25) || 0;
            const median = d3.quantile(sortedValues, 0.5) || 0;
            const q3 = d3.quantile(sortedValues, 0.75) || 0;
            const iqr = q3 - q1;
            const min = Math.max(d3.min(sortedValues) || 0, q1 - 1.5 * iqr);
            const max = Math.min(d3.max(sortedValues) || 0, q3 + 1.5 * iqr);
            return {
                key: key,
                min: min,
                q1: q1,
                median: median,
                q3: q3,
                max: max,
                outliers: sortedValues.filter((v: number) => v < min || v > max)
            };
        });
        console.log(`[${this.container.id}] Data loaded for boxplot:`, this.chartData.length, 'groups');
    }

    setupSvgWithDimensions(chartContent: any) {
        if (!chartContent) return null;
        d3.select(chartContent).selectAll("*").remove();
        const containerDimensions = this.getContainerDimensions();
        this.width = containerDimensions.width;
        this.height = containerDimensions.height;
        this.svg = d3.select(chartContent)
            .append('svg')
            .attr('width', this.width)
            .attr('height', this.height);
        this.g = this.svg.append('g').attr('transform', `translate(${this.margin.left},${this.margin.top})`);
        this.clipPathManager = new ClipPathManager(this.svg);
        return this.svg;
    }

    async render() {
        console.log(`[${this.container.id}] Rendering boxplot...`);
        await this.loadData();
        const chartContent = this.container.querySelector('.chart-content');
        if (!chartContent) {
            console.error(`[${this.container.id}] Boxplot content area not found.`);
            return;
        }
        this.setupSvgWithDimensions(chartContent);

        // Handle empty data case
        if (!this.chartData || this.chartData.length === 0) {
            console.log(`[${this.container.id}] No data to render boxplot, skipping chart rendering`);
            this.g.append('text')
                .attr('x', (this.width - this.margin.left - this.margin.right) / 2)
                .attr('y', (this.height - this.margin.top - this.margin.bottom) / 2)
                .attr('text-anchor', 'middle')
                .style('font-size', '14px')
                .style('fill', '#666')
                .text('No data available');
            return;
        }

        const chartWidth = this.width - this.margin.left - this.margin.right;
        const chartHeight = this.height - this.margin.top - this.margin.bottom;

        this.xScale = d3.scaleBand()
            .domain(this.chartData.map(d => d.key))
            .range([0, chartWidth])
            .padding(0.2);

        const allValues = this.chartData.flatMap(d => [d.min, d.max, ...d.outliers]);
        this.yScale = d3.scaleLinear()
            .domain(d3.extent(allValues) as [number, number])
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

        this.drawBoxplots(this.g, this.chartData, 1.0);

        this.g.append('g')
            .attr('transform', `translate(0,${chartHeight})`)
            .call(d3.axisBottom(this.xScale))
            .selectAll('text')
            .style('text-anchor', 'end')
            .attr('dx', '-.8em')
            .attr('dy', '.15em')
            .attr('transform', 'rotate(-45)')
            .text((d: any, i: number) => {
              if (i % 2 !== 0) return '';
              return truncate(String(d), 10);
            });

        this.g.append('g')
            .call(d3.axisLeft(this.yScale).tickFormat((d: any) => truncate(String(d), 10)));

        this.g.append('g')
            .attr('class', 'grid')
            .call(d3.axisLeft(this.yScale).tickSize(-chartWidth).tickFormat('' as any))
            .call((g: any) => g.select('.domain').remove())
            .selectAll('line')
            .attr('stroke', 'rgba(0,0,0,0.1)');
    }

    handleBrush(selection: any) {
        console.log(`[${this.container.id}] Boxplot brush event handled.`);
        this.g.selectAll('.boxplot-dimmed').remove();
        this.g.selectAll('.boxplot-highlight').remove();
        this.clipPathManager.removeAllClips();

        if (!selection) {
            console.log(`[${this.container.id}] No selection, clearing filters.`);
            this.g.selectAll('.boxplot-group').style('opacity', 1);
            if (this.filterManager) {
                this.filterManager.clearFiltersForChart(this);
            }
            return;
        }

        this.g.selectAll('.boxplot-group').style('opacity', 0);
        this.drawBoxplots(this.g.append('g').attr('class', 'boxplot-dimmed'), this.chartData, 0.2);

        const [[x0, y0], [x1, y1]] = selection;
        const clipUrl = this.clipPathManager.createRectClip(x0, y0, x1 - x0, y1 - y0);
        const highlightGroup = this.g.append('g').attr('class', 'boxplot-highlight').attr('clip-path', clipUrl);
        this.drawBoxplots(highlightGroup, this.chartData, 1.0);

        // Re-append grid lines to ensure they are on top
        const chartWidth = this.width - this.margin.left - this.margin.right;
        this.g.append('g')
            .attr('class', 'grid')
            .call(d3.axisLeft(this.yScale).tickSize(-chartWidth).tickFormat('' as any))
            .call((g: any) => g.select('.domain').remove())
            .selectAll('line')
            .attr('stroke', 'rgba(0,0,0,0.1)');

        // Apply filter through FilterManager if we have a selection
        if (this.filterManager && selection) {
            console.log(`[${this.container.id}] Applying boxplot filter for brush selection`);

            // Convert brush coordinates to data values
            const minY = this.yScale.invert(y1); // y1 is top (smaller y value)
            const maxY = this.yScale.invert(y0); // y0 is bottom (larger y value)

            // Get selected categories based on x-axis brush
            const selectedCategories: string[] = [];
            this.chartData.forEach(d => {
                const categoryX = this.xScale(d.key);
                const categoryWidth = this.xScale.bandwidth();
                if (categoryX !== undefined && x0 < categoryX + categoryWidth && x1 > categoryX) {
                    selectedCategories.push(d.key);
                }
            });

            const filters: any = {};

            // Add Y-axis range filter
            filters[this.yColumnName] = {
                type: 'AND',
                conditions: [
                    { type: 'RANGE', operator: '>=', value: minY },
                    { type: 'RANGE', operator: '<=', value: maxY }
                ],
                raw: `${this.yColumnName}: ${minY.toFixed(2)} - ${maxY.toFixed(2)}`
            };

            // Add X-axis category filter if specific categories are selected
            if (selectedCategories.length > 0 && selectedCategories.length < this.chartData.length) {
                filters[this.xColumnName] = {
                    type: 'IN',
                    values: new Set(selectedCategories),
                    raw: `${this.xColumnName} IN (${selectedCategories.join(', ')})`
                };
            }

            this.filterManager.setFilters(filters, this);
        }
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

        // Whiskers
        boxplotGroups.append('line')
            .attr('x1', boxWidth / 2)
            .attr('x2', boxWidth / 2)
            .attr('y1', (d: any) => this.yScale(d.min))
            .attr('y2', (d: any) => this.yScale(d.max))
            .attr('stroke', 'black');

        // Box
        boxplotGroups.append('rect')
            .attr('x', 0)
            .attr('y', (d: any) => this.yScale(d.q3))
            .attr('width', boxWidth)
            .attr('height', (d: any) => this.yScale(d.q1) - this.yScale(d.q3))
            .attr('stroke', 'black')
            .style('fill', 'steelblue');

        // Median
        boxplotGroups.append('line')
            .attr('x1', 0)
            .attr('x2', boxWidth)
            .attr('y1', (d: any) => this.yScale(d.median))
            .attr('y2', (d: any) => this.yScale(d.median))
            .attr('stroke', 'black')
            .style('stroke-width', 2);

        // Outliers
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

    redrawWithoutRefetch() {
        this.render();
    }

    destroy() {}
}

export class D3BoxplotChartComponent implements m.Component<{
    dataProvider: any,
    xColumnName: string,
    yColumnName: string,
    filterManager?: any,
}> {
    chart: D3BoxplotChart|null = null;
    dom: HTMLElement | null = null;

    oncreate(vnode: m.VnodeDOM<{
        dataProvider: any,
        xColumnName: string,
        yColumnName: string,
        filterManager?: any,
    }>) {
        this.dom = vnode.dom as HTMLElement;
        this.dom.id = `d3-boxplot-chart-${Math.random().toString(36).substr(2, 9)}`;

        this.chart = new D3BoxplotChart(this.dom, {
            ...vnode.attrs,
            filterManager: vnode.attrs.filterManager // Ensure FilterManager is passed to D3 class
        });

        if (vnode.attrs.filterManager) {
            vnode.attrs.filterManager.subscribe(this);
        }
        this.chart.render();
        ComponentManager.register(this);
        ChartManager.register(this);
    }

    async refreshFilteredData() {
        console.log(`[${this.chart?.container.id}] Boxplot refreshFilteredData called`);
        if (this.chart) {
            await this.chart.render();
            m.redraw();
        }
    }

    // Required method for FilterManager integration
    setIsFilterSource(isSource: boolean) {
        console.log(`[${this.chart?.container.id}] Boxplot setIsFilterSource called with:`, isSource);
        if (this.chart) {
            this.chart.isFilterSource = isSource;
            // Update visual styling to indicate filter source
            if (this.dom) {
                const header = this.dom.querySelector('.chart-header');
                if (header) {
                    header.classList.toggle('filter-source', isSource);
                }
            }

            // Auto-refresh when becoming filter source if flag is enabled
            if (isSource && (window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER === true) {
                this.refreshFilteredData();
            }
        }
    }

    onremove(vnode: any) {
        if (vnode.attrs.filterManager) {
            vnode.attrs.filterManager.unsubscribe(this);
        }
        if (this.chart) {
          this.chart.destroy();
        }
        ChartManager.unregister(this);
      }

    view() {
        return m(
            '.chart-container',
            {style: {width: '500px', height: '400px'}},
            m('.chart-header',
              {
                onclick: (event: MouseEvent) => {
                  event.stopPropagation();
                  if (this.chart?.filterManager) {
                    this.chart.filterManager.clearFiltersForChart(this.chart);
                  }
                },
              },
              m('h4.chart-title', 'Boxplot'),
              m('.chart-actions',
                m('button.chart-action-btn', {title: 'Duplicate chart', onclick: () => ChartManager.duplicateChart(this.dom)}, '⧉'),
                m('button.chart-close-btn', {title: 'Remove Chart', onclick: () => ChartManager.removeChart(this.dom)}, '×'),
              ),
            ),
            m('.chart-content', {style: {'overflow-x': 'auto', 'overflow-y': 'hidden'}}),
            m('.resize-handle', {
              onmousedown: (e: MouseEvent) => {
                if (this.dom) {
                  ResizeManager.getInstance().onMouseDown(
                      e, this.dom, this.dom);
                }
              },
            }),
        );
    }
}

class D3ViolinPlotChart {
    container: HTMLElement;
    width: number = 600;
    height: number = 400;
    margin = { top: 10, right: 30, bottom: 150, left: 60 };
    svg: any;
    g: any;
    chartData: any[] = [];
    xColumnName: string;
    yColumnName: string;
    dataProvider: any;
    xScale: any;
    yScale: any;
    clipPathManager!: ClipPathManager;
    filterManager: any;
    isFilterSource: boolean = false;

    constructor(container: HTMLElement, attrs: any) {
        this.container = container;
        this.dataProvider = attrs.dataProvider;
        this.xColumnName = attrs.xColumnName;
        this.yColumnName = attrs.yColumnName;
        this.filterManager = attrs.filterManager;
    }

    getContainerDimensions() {
        const rect = this.container.getBoundingClientRect();
        return {
            width: rect.width || 600,
            height: rect.height || 400,
        };
    }

    async loadData() {
        console.log(`[${this.container.id}] Loading data for violin plot...`);
        console.log(`[${this.container.id}] FilterManager active filters:`, this.filterManager ? this.filterManager.getFilters() : 'No FilterManager');

        const querySpec = {
          filters: this.filterManager ? this.filterManager.getFiltersForQuery() : [],
        };
        const result = await this.dataProvider.query(querySpec);
        const data = result.data;

        const groupedData = d3.group(data, (d: any) => d[this.xColumnName]);
        this.chartData = Array.from(groupedData, ([key, values]) => {
            const sortedValues = values.map((d: any) => +d[this.yColumnName]).sort(d3.ascending);
            const density = this.kernelDensityEstimator(this.kernelEpanechnikov(7), this.yScale.ticks(40))(sortedValues);
            const min = d3.min(sortedValues) || 0;
            const median = d3.quantile(sortedValues, 0.5) || 0;
            const p90 = d3.quantile(sortedValues, 0.90) || 0;
            const p95 = d3.quantile(sortedValues, 0.95) || 0;
            const p99 = d3.quantile(sortedValues, 0.99) || 0;
            const q1 = d3.quantile(sortedValues, 0.25) || 0;
            const q3 = d3.quantile(sortedValues, 0.75) || 0;
            return {
                key: key,
                density: density,
                min: min,
                median: median,
                p90: p90,
                p95: p95,
                p99: p99,
                q1: q1,
                q3: q3
            };
        });
        console.log(`[${this.container.id}] Data loaded for violin plot:`, this.chartData.length, 'groups');
    }

    setupSvgWithDimensions(chartContent: any) {
        if (!chartContent) return null;
        d3.select(chartContent).selectAll("*").remove();
        const containerDimensions = this.getContainerDimensions();
        this.width = containerDimensions.width;
        this.height = containerDimensions.height;
        this.svg = d3.select(chartContent)
            .append('svg')
            .attr('width', this.width)
            .attr('height', this.height);
        this.g = this.svg.append('g').attr('transform', `translate(${this.margin.left},${this.margin.top})`);
        this.clipPathManager = new ClipPathManager(this.svg);
        return this.svg;
    }

    async render() {
        console.log(`[${this.container.id}] Rendering violin plot...`);
        const chartContent = this.container.querySelector('.chart-content');
        if (!chartContent) {
            console.error(`[${this.container.id}] Violin plot content area not found.`);
            return;
        }

        // Initial setup to get yScale for density calculation
        this.setupSvgWithDimensions(chartContent);
        const chartHeight = this.height - this.margin.top - this.margin.bottom;

        const preliminaryData = await this.dataProvider.query({});
        const allYValues = preliminaryData.data.map((d:any) => +d[this.yColumnName]).filter((v: any) => !isNaN(v));

        const yExtent = d3.extent(allYValues);
        this.yScale = d3.scaleLinear()
            .domain([Number(yExtent[0]) || 0, Number(yExtent[1]) || 0])
            .range([chartHeight, 0])
            .nice();

        await this.loadData();

        // Handle empty data case
        if (!this.chartData || this.chartData.length === 0) {
            console.log(`[${this.container.id}] No data to render violin plot, skipping chart rendering`);
            this.g.append('text')
                .attr('x', (this.width - this.margin.left - this.margin.right) / 2)
                .attr('y', (this.height - this.margin.top - this.margin.bottom) / 2)
                .attr('text-anchor', 'middle')
                .style('font-size', '14px')
                .style('fill', '#666')
                .text('No data available');
            return;
        }

        const chartWidth = this.width - this.margin.left - this.margin.right;

        this.xScale = d3.scaleBand()
            .domain(this.chartData.map(d => d.key))
            .range([0, chartWidth])
            .padding(0.05);

        const brush = d3.brush()
            .extent([[0, 0], [chartWidth, chartHeight]])
            .on('end', (event: { selection: any; }) => {
                this.handleBrush(event.selection);
            });

        this.g.append('g')
            .attr('class', 'brush')
            .call(brush);

        this.drawViolins(this.g, this.chartData, 1.0);

        this.g.append('g')
            .attr('transform', `translate(0,${chartHeight})`)
            .call(d3.axisBottom(this.xScale))
            .selectAll('text')
            .style('text-anchor', 'end')
            .attr('dx', '-.8em')
            .attr('dy', '.15em')
            .attr('transform', 'rotate(-45)')
            .text((d: any, i: number) => {
              if (i % 2 !== 0) return '';
              return truncate(String(d), 10);
            });

        this.g.append('g')
            .call(d3.axisLeft(this.yScale).tickFormat((d: any) => truncate(String(d), 10)));

        this.g.append('g')
            .attr('class', 'grid')
            .call(d3.axisLeft(this.yScale).tickSize(-chartWidth).tickFormat('' as any))
            .call((g: any) => g.select('.domain').remove())
            .selectAll('line')
            .attr('stroke', 'rgba(0,0,0,0.1)');
    }

    handleBrush(selection: any) {
        console.log(`[${this.container.id}] Violin plot brush event handled.`);
        this.g.selectAll('.violin-dimmed').remove();
        this.g.selectAll('.violin-highlight').remove();
        this.clipPathManager.removeAllClips();

        if (!selection) {
            console.log(`[${this.container.id}] No selection, clearing filters.`);
            this.g.selectAll('.violin-group').style('opacity', 1);
            if (this.filterManager) {
                this.filterManager.clearFiltersForChart(this);
            }
            return;
        }

        this.g.selectAll('.violin-group').style('opacity', 0);
        this.drawViolins(this.g.append('g').attr('class', 'violin-dimmed'), this.chartData, 0.2);

        const [[x0, y0], [x1, y1]] = selection;
        const clipUrl = this.clipPathManager.createRectClip(x0, y0, x1 - x0, y1 - y0);
        const highlightGroup = this.g.append('g').attr('class', 'violin-highlight').attr('clip-path', clipUrl);
        this.drawViolins(highlightGroup, this.chartData, 1.0);

        // Re-append grid lines to ensure they are on top
        const chartWidth = this.width - this.margin.left - this.margin.right;
        this.g.append('g')
            .attr('class', 'grid')
            .call(d3.axisLeft(this.yScale).tickSize(-chartWidth).tickFormat('' as any))
            .call((g: any) => g.select('.domain').remove())
            .selectAll('line')
            .attr('stroke', 'rgba(0,0,0,0.1)');

        // Apply filter through FilterManager if we have a selection
        if (this.filterManager && selection) {
            console.log(`[${this.container.id}] Applying violin plot filter for brush selection`);

            // Convert brush coordinates to data values
            const minY = this.yScale.invert(y1); // y1 is top (smaller y value)
            const maxY = this.yScale.invert(y0); // y0 is bottom (larger y value)

            // Get selected categories based on x-axis brush
            const selectedCategories: string[] = [];
            this.chartData.forEach(d => {
                const categoryX = this.xScale(d.key);
                const categoryWidth = this.xScale.bandwidth();
                if (categoryX !== undefined && x0 < categoryX + categoryWidth && x1 > categoryX) {
                    selectedCategories.push(d.key);
                }
            });

            const filters: any = {};

            // Add Y-axis range filter
            filters[this.yColumnName] = {
                type: 'AND',
                conditions: [
                    { type: 'RANGE', operator: '>=', value: minY },
                    { type: 'RANGE', operator: '<=', value: maxY }
                ],
                raw: `${this.yColumnName}: ${minY.toFixed(2)} - ${maxY.toFixed(2)}`
            };

            // Add X-axis category filter if specific categories are selected
            if (selectedCategories.length > 0 && selectedCategories.length < this.chartData.length) {
                filters[this.xColumnName] = {
                    type: 'IN',
                    values: new Set(selectedCategories),
                    raw: `${this.xColumnName} IN (${selectedCategories.join(', ')})`
                };
            }

            this.filterManager.setFilters(filters, this);
        }
    }

    drawViolins(container: any, data: any[], opacity: number) {
        const maxDensity = d3.max(data, (d: any) => d3.max(d.density, (p: any) => p[1]));
        const xNum = d3.scaleLinear()
            .domain([0, +maxDensity!])
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
            .attr('transform', (d: any) => `translate(${this.xScale(d.key) + this.xScale.bandwidth() / 2}, 0)`)
            .style('opacity', opacity);

        violinGroups.append('path')
            .datum((d: any) => d.density)
            .attr('d', area)
            .style('fill', 'steelblue');

        // IQR line
        violinGroups.append('line')
            .attr('x1', 0)
            .attr('x2', 0)
            .attr('y1', (d: any) => this.yScale(d.q1))
            .attr('y2', (d: any) => this.yScale(d.q3))
            .attr('stroke', 'black')
            .style('stroke-width', 2);


        // Median dot
        violinGroups.append('circle')
            .attr('cx', 0)
            .attr('cy', (d: any) => this.yScale(d.median))
            .attr('r', 3)
            .style('fill', 'white');

        // p90 dot
        violinGroups.append('circle')
            .attr('cx', 0)
            .attr('cy', (d: any) => this.yScale(d.p90))
            .attr('r', 3)
            .style('fill', 'orange');

        // p95 dot
        violinGroups.append('circle')
            .attr('cx', 0)
            .attr('cy', (d: any) => this.yScale(d.p95))
            .attr('r', 3)
            .style('fill', 'red');

        // p99 dot
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

    kernelDensityEstimator(kernel: (v: number) => number, X: number[]) {
        return function(V: number[]) {
            return X.map(function(x) {
                return [x, d3.mean(V, function(v: any) { return kernel(x - v); })];
            });
        };
    }

    kernelEpanechnikov(k: number) {
        return function(v: number) {
            return Math.abs(v /= k) <= 1 ? 0.75 * (1 - v * v) / k : 0;
        };
    }

    redrawWithoutRefetch() {
        this.render();
    }

    destroy() {}
}

export class D3ViolinPlotChartComponent implements m.Component<{
    dataProvider: any,
    xColumnName: string,
    yColumnName: string,
    filterManager?: any,
}> {
    chart: D3ViolinPlotChart|null = null;
    dom: HTMLElement | null = null;

    oncreate(vnode: m.VnodeDOM<{
        dataProvider: any,
        xColumnName: string,
        yColumnName: string,
        filterManager?: any,
    }>) {
        this.dom = vnode.dom as HTMLElement;
        this.dom.id = `d3-violin-plot-chart-${Math.random().toString(36).substr(2, 9)}`;

        this.chart = new D3ViolinPlotChart(this.dom, vnode.attrs);
        if (vnode.attrs.filterManager) {
            vnode.attrs.filterManager.subscribe(this);
        }
        this.chart.render();
        ComponentManager.register(this);
        ChartManager.register(this);
    }

    async refreshFilteredData() {
        console.log(`[${this.chart?.container.id}] Violin plot refreshFilteredData called`);
        if (this.chart) {
            await this.chart.render();
            m.redraw();
        }
    }

    // Required method for FilterManager integration
    setIsFilterSource(isSource: boolean) {
        console.log(`[${this.chart?.container.id}] Violin plot setIsFilterSource called with:`, isSource);
        if (this.chart) {
            this.chart.isFilterSource = isSource;
            // Update visual styling to indicate filter source
            if (this.dom) {
                const header = this.dom.querySelector('.chart-header');
                if (header) {
                    header.classList.toggle('filter-source', isSource);
                }
            }

            // Auto-refresh when becoming filter source if flag is enabled
            if (isSource && (window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER === true) {
                this.refreshFilteredData();
            }
        }
    }

    onremove(vnode: any) {
        if (vnode.attrs.filterManager) {
            vnode.attrs.filterManager.unsubscribe(this);
        }
        if (this.chart) {
          this.chart.destroy();
        }
        ChartManager.unregister(this);
      }

    view() {
        return m(
            '.chart-container',
            {style: {width: '500px', height: '400px'}},
            m('.chart-header',
              {
                onclick: (event: MouseEvent) => {
                  event.stopPropagation();
                  if (this.chart?.filterManager) {
                    this.chart.filterManager.clearFiltersForChart(this.chart);
                  }
                },
              },
              m('h4.chart-title', 'Violin Plot'),
              m('.chart-actions',
                m('button.chart-action-btn', {title: 'Duplicate chart', onclick: () => ChartManager.duplicateChart(this.dom)}, '⧉'),
                m('button.chart-close-btn', {title: 'Remove Chart', onclick: () => ChartManager.removeChart(this.dom)}, '×'),
              ),
            ),
            m('.chart-content', {style: {'overflow-x': 'auto', 'overflow-y': 'hidden'}}),
            m('.resize-handle', {
              onmousedown: (e: MouseEvent) => {
                if (this.dom) {
                  ResizeManager.getInstance().onMouseDown(
                      e, this.dom, this.dom);
                }
              },
            }),
        );
    }
}

class D3LineChart {
    container: HTMLElement;
    width: number = 600;
    height: number = 400;
    margin = { top: 10, right: 180, bottom: 150, left: 60 };
    svg: any;
    g: any;
    chartData: any[] = [];
    xColumnName: string;
    yColumnName: string;
    colorBy: string | null;
    aggregationFunction: string;
    dataProvider: any;
    xScale: any;
    yScale: any;
    colorScale: any;
    line: any;
    clipPathManager!: ClipPathManager;
    filterManager: any;
    isFilterSource: boolean = false;

    constructor(container: HTMLElement, attrs: any) {
        this.container = container;
        this.dataProvider = attrs.dataProvider;
        this.xColumnName = attrs.xColumnName;
        this.yColumnName = attrs.yColumnName;
        this.colorBy = attrs.colorBy || null;
        this.aggregationFunction = attrs.aggregationFunction;
        this.filterManager = attrs.filterManager;
    }

    getContainerDimensions() {
        const rect = this.container.getBoundingClientRect();
        return {
            width: rect.width || 600,
            height: rect.height || 400,
        };
    }

    async loadData() {
        console.log(`[${this.container.id}] Loading data for line chart...`);
        console.log(`[${this.container.id}] FilterManager active filters:`, this.filterManager ? this.filterManager.getFilters() : 'No FilterManager');

        const querySpec: any = {
          filters: this.filterManager ? this.filterManager.getFiltersForQuery() : [],
        };

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

        const result = await this.dataProvider.query(querySpec);
        this.chartData = result.data;

        console.log(`[${this.container.id}] Data loaded for line chart:`, this.chartData.length, 'rows');
    }

    setupSvgWithDimensions(chartContent: any) {
        if (!chartContent) return null;
        d3.select(chartContent).selectAll("*").remove();
        const containerDimensions = this.getContainerDimensions();
        this.width = containerDimensions.width;
        this.height = containerDimensions.height;
        this.svg = d3.select(chartContent)
            .append('svg')
            .attr('width', this.width)
            .attr('height', this.height);
        this.g = this.svg.append('g').attr('transform', `translate(${this.margin.left},${this.margin.top})`);
        this.clipPathManager = new ClipPathManager(this.svg);
        return this.svg;
    }

    async render() {
        console.log(`[${this.container.id}] Rendering line chart...`);
        await this.loadData();
        const chartContent = this.container.querySelector('.chart-content');
        if (!chartContent) {
            console.error(`[${this.container.id}] Line chart content area not found.`);
            return;
        }
        this.setupSvgWithDimensions(chartContent);

        // Handle empty data case
        if (!this.chartData || this.chartData.length === 0) {
            console.log(`[${this.container.id}] No data to render line chart, skipping chart rendering`);
            this.g.append('text')
                .attr('x', (this.width - this.margin.left - this.margin.right) / 2)
                .attr('y', (this.height - this.margin.top - this.margin.bottom) / 2)
                .attr('text-anchor', 'middle')
                .style('font-size', '14px')
                .style('fill', '#666')
                .text('No data available');
            return;
        }

        const chartWidth = this.width - this.margin.left - this.margin.right;
        const chartHeight = this.height - this.margin.top - this.margin.bottom;

        const yValueAccessor = (d: any) => this.aggregationFunction ? d.__aggregated_value : +d[this.yColumnName];
        const xData = this.chartData.map((d: any) => +d[this.xColumnName]).filter(v => !isNaN(v));
        const yData = this.chartData.map(yValueAccessor).filter(v => !isNaN(v));

        // Handle case where all values are filtered out
        if (xData.length === 0 || yData.length === 0) {
            console.log(`[${this.container.id}] No valid numeric values for line chart`);
            this.g.append('text')
                .attr('x', chartWidth / 2)
                .attr('y', chartHeight / 2)
                .attr('text-anchor', 'middle')
                .style('font-size', '14px')
                .style('fill', '#666')
                .text('No valid data for line chart');
            return;
        }

        this.xScale = d3.scaleLinear()
            .domain(d3.extent(xData) as [number, number])
            .range([0, chartWidth])
            .nice();

        this.yScale = d3.scaleLinear()
            .domain(d3.extent(yData) as [number, number])
            .range([chartHeight, 0])
            .nice();

        this.line = d3.line()
            .x((d: any) => this.xScale(+d[this.xColumnName]))
            .y((d: any) => this.yScale(yValueAccessor(d)));

        if (this.colorBy) {
            const colorValues = [...new Set(this.chartData.map((d: any) => d[this.colorBy!]))];
            this.colorScale = d3.scaleOrdinal(d3.schemeCategory10).domain(colorValues);
        }

        const brush = d3.brush()
            .extent([[0, 0], [chartWidth, chartHeight]])
            .on('end', (event: { selection: any; }) => {
                this.handleBrush(event.selection);
            });

        this.g.append('g')
            .attr('class', 'brush')
            .call(brush);

        this.drawLines(this.g, this.chartData, 1.0);

        this.g.append('g')
            .attr('transform', `translate(0,${chartHeight})`)
            .call(d3.axisBottom(this.xScale))
            .selectAll('text')
            .style('text-anchor', 'end')
            .attr('dx', '-.8em')
            .attr('dy', '.15em')
            .attr('transform', 'rotate(-45)')
            .text((d: any, i: number) => {
              if (i % 2 !== 0) return '';
              return truncate(String(d), 10);
            });

        this.g.append('g')
            .call(d3.axisLeft(this.yScale).tickFormat((d: any) => truncate(String(d), 10)));

        this.g.append('g')
            .attr('class', 'grid')
            .call(d3.axisLeft(this.yScale).tickSize(-chartWidth).tickFormat('' as any))
            .call((g: any) => g.select('.domain').remove())
            .selectAll('line')
            .attr('stroke', 'rgba(0,0,0,0.1)');
        if (this.colorBy) {
            this.renderLegend();
        }
    }

    handleBrush(selection: any) {
        console.log(`[${this.container.id}] Line chart brush event handled.`);
        this.g.selectAll('.line-dimmed').remove();
        this.g.selectAll('.line-highlight').remove();
        this.clipPathManager.removeAllClips();

        if (!selection) {
            console.log(`[${this.container.id}] No selection, clearing filters.`);
            this.g.selectAll('.line').style('opacity', 1);
            if (this.filterManager) {
                this.filterManager.clearFiltersForChart(this);
            }
            return;
        }

        this.g.selectAll('.line').style('opacity', 0);
        this.drawLines(this.g.append('g').attr('class', 'line-dimmed'), this.chartData, 0.2);

        const [[x0, y0], [x1, y1]] = selection;
        const clipUrl = this.clipPathManager.createRectClip(x0, y0, x1 - x0, y1 - y0);
        const highlightGroup = this.g.append('g').attr('class', 'line-highlight').attr('clip-path', clipUrl);
        this.drawLines(highlightGroup, this.chartData, 1.0);

        // Apply filter through FilterManager if we have a selection
        if (this.filterManager && selection) {
            console.log(`[${this.container.id}] Applying line chart filter for brush selection`);

            // Convert brush coordinates to data values
            const minX = this.xScale.invert(x0);
            const maxX = this.xScale.invert(x1);
            const minY = this.yScale.invert(y1); // y1 is top (smaller y value)
            const maxY = this.yScale.invert(y0); // y0 is bottom (larger y value)

            const filters: any = {};

            // Add X-axis range filter
            filters[this.xColumnName] = {
                type: 'AND',
                conditions: [
                    { type: 'RANGE', operator: '>=', value: minX },
                    { type: 'RANGE', operator: '<=', value: maxX }
                ],
                raw: `${this.xColumnName}: ${minX.toFixed(2)} - ${maxX.toFixed(2)}`
            };

            // Add Y-axis range filter
            filters[this.yColumnName] = {
                type: 'AND',
                conditions: [
                    { type: 'RANGE', operator: '>=', value: minY },
                    { type: 'RANGE', operator: '<=', value: maxY }
                ],
                raw: `${this.yColumnName}: ${minY.toFixed(2)} - ${maxY.toFixed(2)}`
            };

            this.filterManager.setFilters(filters, this);
        }
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

    redrawWithoutRefetch() {
        this.render();
    }

    destroy() {}

    renderLegend() {
        const chartContent = this.container.querySelector('.chart-content');
        if (!chartContent) return;
        d3.select(chartContent).select('.chart-legend').remove();

        const legendContainer = d3.select(chartContent).append('div').attr('class', 'chart-legend');

        const maxLegendItems = 10;
        const legendData = this.colorScale.domain();
        const truncated = legendData.length > maxLegendItems;
        const data = truncated ? legendData.slice(0, maxLegendItems) : legendData;

        const legendItems = legendContainer.selectAll('.legend-item')
            .data(data)
            .enter()
            .append('div')
            .attr('class', 'legend-item')
            .on('click', (_event: MouseEvent, d: any) => {
                if (this.filterManager && this.colorBy) {
                    this.filterManager.setFilters(
                        {[this.colorBy]: {type: "IN", values: new Set([d])}},
                        this
                    );
                }
            });

        legendItems.append('span')
            .attr('class', 'legend-swatch')
            .style('background-color', (d: any) => this.colorScale(d));

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

export class D3LineChartComponent implements m.Component<{
    dataProvider: any,
    xColumnName: string,
    yColumnName: string,
    colorBy?: string,
    filterManager?: any,
}> {
    chart: D3LineChart|null = null;
    dom: HTMLElement | null = null;

    oncreate(vnode: m.VnodeDOM<{
        dataProvider: any,
        xColumnName: string,
        yColumnName: string,
        colorBy?: string,
        filterManager?: any,
    }>) {
        this.dom = vnode.dom as HTMLElement;
        this.dom.id = `d3-line-chart-${Math.random().toString(36).substr(2, 9)}`;

        this.chart = new D3LineChart(this.dom, vnode.attrs);
        if (vnode.attrs.filterManager) {
            vnode.attrs.filterManager.subscribe(this);
        }
        this.chart.render();
        ComponentManager.register(this);
        ChartManager.register(this);
    }

    async refreshFilteredData() {
        console.log(`[${this.chart?.container.id}] Line chart refreshFilteredData called`);
        if (this.chart) {
            await this.chart.render();
            m.redraw();
        }
    }

    // Required method for FilterManager integration
    setIsFilterSource(isSource: boolean) {
        console.log(`[${this.chart?.container.id}] Line chart setIsFilterSource called with:`, isSource);
        if (this.chart) {
            this.chart.isFilterSource = isSource;
            // Update visual styling to indicate filter source
            if (this.dom) {
                const header = this.dom.querySelector('.chart-header');
                if (header) {
                    header.classList.toggle('filter-source', isSource);
                }
            }

            // Auto-refresh when becoming filter source if flag is enabled
            if (isSource && (window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER === true) {
                this.refreshFilteredData();
            }
        }
    }

    onremove(vnode: any) {
        if (vnode.attrs.filterManager) {
            vnode.attrs.filterManager.unsubscribe(this);
        }
        if (this.chart) {
          this.chart.destroy();
        }
        ChartManager.unregister(this);
      }

    view() {
        return m(
            '.chart-container',
            {style: {width: '500px', height: '400px'}},
            m('.chart-header',
              {
                onclick: (event: MouseEvent) => {
                  event.stopPropagation();
                  if (this.chart?.filterManager) {
                    this.chart.filterManager.clearFiltersForChart(this.chart);
                  }
                },
              },
              m('h4.chart-title', 'Line Chart'),
              m('.chart-actions',
                m('button.chart-action-btn', {title: 'Duplicate chart', onclick: () => ChartManager.duplicateChart(this.dom)}, '⧉'),
                m('button.chart-close-btn', {title: 'Remove Chart', onclick: () => ChartManager.removeChart(this.dom)}, '×'),
              ),
            ),
            m('.chart-content'),
            m('.resize-handle', {
              onmousedown: (e: MouseEvent) => {
                if (this.dom) {
                  ResizeManager.getInstance().onMouseDown(
                      e, this.dom, this.dom);
                }
              },
            }),
        );
    }
}

class D3DonutChart {
    container: HTMLElement;
    width: number = 600;
    height: number = 400;
    margin = { top: 10, right: 180, bottom: 90, left: 60 };
    svg: any;
    g: any;
    chartData: any[] = [];
    valueColumnName: string;
    categoryColumnName: string;
    aggregationFunction: string;
    dataProvider: any;
    colorScale: any;
    filterManager: any;
    isFilterSource: boolean = false;

    constructor(container: HTMLElement, attrs: any) {
        this.container = container;
        this.dataProvider = attrs.dataProvider;
        this.valueColumnName = attrs.valueColumnName;
        this.categoryColumnName = attrs.categoryColumnName;
        this.aggregationFunction = attrs.aggregationFunction || 'sum';
        this.filterManager = attrs.filterManager;
    }

    getContainerDimensions() {
        const rect = this.container.getBoundingClientRect();
        return {
            width: rect.width || 600,
            height: rect.height || 400,
        };
    }

    async loadData() {
        console.log(`[${this.container.id}] Loading data for donut chart...`);
        console.log(`[${this.container.id}] FilterManager active filters:`, this.filterManager ? this.filterManager.getFilters() : 'No FilterManager');

        const querySpec = {
            aggregation: {
                groupBy: this.categoryColumnName,
                field: this.valueColumnName,
                function: this.aggregationFunction
            }
        };
        const querySpecWithFilters = {
          ...querySpec,
          filters: this.filterManager ? this.filterManager.getFiltersForQuery() : [],
        };
        const result = await this.dataProvider.query(querySpecWithFilters);
        this.chartData = result.data;

        console.log(`[${this.container.id}] Data loaded for donut chart:`, this.chartData.length, 'categories');
    }

    setupSvgWithDimensions(chartContent: any) {
        if (!chartContent) return null;
        d3.select(chartContent).selectAll("*").remove();
        const containerDimensions = this.getContainerDimensions();
        this.width = containerDimensions.width;
        this.height = containerDimensions.height;
        this.svg = d3.select(chartContent)
            .append('svg')
            .attr('width', this.width)
            .attr('height', this.height)
            .on('click', () => {
                console.log(`[${this.container.id}] Clicked outside donut chart, clearing selection and filters`);
                this.g.selectAll('.arc').classed('selected', false).style('opacity', 1.0);

                // Clear filters when clicking outside
                if (this.filterManager) {
                    this.filterManager.clearFiltersForChart(this);
                }
            });
        this.g = this.svg.append('g').attr('transform', `translate(${this.width / 2},${this.height / 2 - this.margin.top * 2})`);
        return this.svg;
    }

    async render() {
        console.log(`[${this.container.id}] Rendering donut chart...`);
        await this.loadData();
        const chartContent = this.container.querySelector('.chart-content');
        if (!chartContent) {
            console.error(`[${this.container.id}] Donut chart content area not found.`);
            return;
        }
        this.setupSvgWithDimensions(chartContent);

        // Handle empty data case
        if (!this.chartData || this.chartData.length === 0) {
            console.log(`[${this.container.id}] No data to render donut chart, skipping chart rendering`);
            this.g.append('text')
                .attr('x', 0)
                .attr('y', 0)
                .attr('text-anchor', 'middle')
                .style('font-size', '14px')
                .style('fill', '#666')
                .text('No data available');
            return;
        }

        const radius = Math.min(this.width - this.margin.left - this.margin.right, this.height - this.margin.top - this.margin.bottom) / 2 - 10;

        const pie = d3.pie()
            .value((d: any) => d.__aggregated_value)
            .sort(null);

        const arc = d3.arc()
            .innerRadius(radius * 0.5)
            .outerRadius(radius);

        const categories = this.chartData.map((d: any) => d[this.categoryColumnName]);
        this.colorScale = d3.scaleOrdinal(d3.schemeCategory10).domain(categories);

        const arcs = this.g.selectAll('.arc')
            .data(pie(this.chartData))
            .enter()
            .append('g')
            .attr('class', 'arc');

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

                // Apply filter through FilterManager if we have selected data
                if (this.filterManager && !isSelected) {
                    console.log(`[${this.container.id}] Applying donut chart filter for clicked slice`);

                    // Get the category value from the clicked slice
                    const categoryValue = d.data[this.categoryColumnName];

                    const filters: any = {};
                    filters[this.categoryColumnName] = {
                        type: 'IN',
                        values: new Set([categoryValue]),
                        raw: `${this.categoryColumnName} = ${categoryValue}`
                    };

                    this.filterManager.setFilters(filters, this);
                } else if (this.filterManager && isSelected) {
                    // Clear filters when deselecting
                    this.filterManager.clearFiltersForChart(this);
                }
            });

        const tooltipManager = TooltipManager.getInstance();
        tooltipManager.addTooltip(paths, (d: any) => {
          return `
            <strong>${this.categoryColumnName}:</strong> ${d.data[this.categoryColumnName]}<br>
            <strong>${this.valueColumnName}:</strong> ${d.data.__aggregated_value}
          `;
        });

        this.renderLegend();
    }

    redrawWithoutRefetch() {
        this.render();
    }

    destroy() {}

    renderLegend() {
        const chartContent = this.container.querySelector('.chart-content');
        if (!chartContent) return;
        d3.select(chartContent).select('.chart-legend').remove();

        const legendContainer = d3.select(chartContent).append('div').attr('class', 'chart-legend');

        const maxLegendItems = 10;
        const legendData = this.colorScale.domain();
        const truncated = legendData.length > maxLegendItems;
        const data = truncated ? legendData.slice(0, maxLegendItems) : legendData;

        const legendItems = legendContainer.selectAll('.legend-item')
            .data(data)
            .enter()
            .append('div')
            .attr('class', 'legend-item')
            .on('click', (_event: MouseEvent, d: any) => {
                if (this.filterManager) {
                    this.filterManager.setFilters(
                        {[this.categoryColumnName]: {type: "IN", values: new Set([d])}},
                        this
                    );
                }
            });

        legendItems.append('span')
            .attr('class', 'legend-swatch')
            .style('background-color', (d: any) => this.colorScale(d));

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

export class D3DonutChartComponent implements m.Component<{
    dataProvider: any,
    valueColumnName: string,
    categoryColumnName: string,
    filterManager?: any,
}> {
    chart: D3DonutChart|null = null;
    dom: HTMLElement | null = null;

    oncreate(vnode: m.VnodeDOM<{
        dataProvider: any,
        valueColumnName: string,
        categoryColumnName: string,
        filterManager?: any,
    }>) {
        this.dom = vnode.dom as HTMLElement;
        this.dom.id = `d3-donut-chart-${Math.random().toString(36).substr(2, 9)}`;

        this.chart = new D3DonutChart(this.dom, vnode.attrs);
        if (vnode.attrs.filterManager) {
            vnode.attrs.filterManager.subscribe(this);
        }
        this.chart.render();
        ComponentManager.register(this);
        ChartManager.register(this);
    }

    async refreshFilteredData() {
        console.log(`[${this.chart?.container.id}] Donut chart refreshFilteredData called`);
        if (this.chart) {
            await this.chart.render();
            m.redraw();
        }
    }

    // Required method for FilterManager integration
    setIsFilterSource(isSource: boolean) {
        console.log(`[${this.chart?.container.id}] Donut chart setIsFilterSource called with:`, isSource);
        if (this.chart) {
            this.chart.isFilterSource = isSource;
            // Update visual styling to indicate filter source
            if (this.dom) {
                const header = this.dom.querySelector('.chart-header');
                if (header) {
                    header.classList.toggle('filter-source', isSource);
                }
            }

            // Auto-refresh when becoming filter source if flag is enabled
            if (isSource && (window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER === true) {
                this.refreshFilteredData();
            }
        }
    }

    onremove(vnode: any) {
        if (vnode.attrs.filterManager) {
            vnode.attrs.filterManager.unsubscribe(this);
        }
        if (this.chart) {
          this.chart.destroy();
        }
        ChartManager.unregister(this);
      }

    view() {
        return m(
            '.chart-container',
            {style: {width: '500px', height: '400px'}},
            m('.chart-header',
              {
                onclick: (event: MouseEvent) => {
                  event.stopPropagation();
                  if (this.chart?.filterManager) {
                    this.chart.filterManager.clearFiltersForChart(this.chart);
                  }
                },
              },
              m('h4.chart-title', 'Donut Chart'),
              m('.chart-actions',
                m('button.chart-action-btn', {title: 'Duplicate chart', onclick: () => ChartManager.duplicateChart(this.dom)}, '⧉'),
                m('button.chart-close-btn', {title: 'Remove Chart', onclick: () => ChartManager.removeChart(this.dom)}, '×'),
              ),
            ),
            m('.chart-content'),
            m('.resize-handle', {
              onmousedown: (e: MouseEvent) => {
                if (this.dom) {
                  ResizeManager.getInstance().onMouseDown(
                      e, this.dom, this.dom);
                }
              },
            }),
          );
    }
}

class D3StackedBarChart {
    container: HTMLElement;
    width: number = 600;
    height: number = 400;
    margin = { top: 10, right: 180, bottom: 150, left: 60 };
    svg: any;
    g: any;
    chartData: any[] = [];
    xColumnName: string;
    yColumnName: string;
    stackColumnName: string;
    aggregationFunction: string;
    dataProvider: any;
    xScale: any;
    yScale: any;
    colorScale: any;
    filterManager: any;
    isFilterSource: boolean = false;

    constructor(container: HTMLElement, attrs: any) {
        this.container = container;
        this.dataProvider = attrs.dataProvider;
        this.xColumnName = attrs.xColumnName;
        this.yColumnName = attrs.yColumnName;
        this.stackColumnName = attrs.stackColumnName;
        this.aggregationFunction = attrs.aggregationFunction;
        this.filterManager = attrs.filterManager;
        if (this.aggregationFunction) {
            console.log('Aggregation function', this.aggregationFunction, 'is not yet implemented for Stacked Bar charts.');
        }
    }

    getContainerDimensions() {
        const rect = this.container.getBoundingClientRect();
        return {
            width: rect.width || 600,
            height: rect.height || 400,
        };
    }

    async loadData() {
        console.log(`[${this.container.id}] Loading data for stacked bar chart...`);
        console.log(`[${this.container.id}] FilterManager active filters:`, this.filterManager ? this.filterManager.getFilters() : 'No FilterManager');

        const querySpec = {
          filters: this.filterManager ? this.filterManager.getFiltersForQuery() : [],
        };
        const result = await this.dataProvider.query(querySpec);
        this.chartData = result.data;

        console.log(`[${this.container.id}] Data loaded for stacked bar chart:`, this.chartData.length, 'rows');
    }

    setupSvgWithDimensions(chartContent: any) {
        if (!chartContent) return null;
        d3.select(chartContent).selectAll("*").remove();
        const containerDimensions = this.getContainerDimensions();
        this.width = containerDimensions.width;
        this.height = containerDimensions.height;
        this.svg = d3.select(chartContent)
            .append('svg')
            .attr('width', this.width)
            .attr('height', this.height);
        this.g = this.svg.append('g').attr('transform', `translate(${this.margin.left},${this.margin.top})`);
        return this.svg;
    }

    async render() {
        console.log(`[${this.container.id}] Rendering stacked bar chart...`);
        await this.loadData();
        const chartContent = this.container.querySelector('.chart-content');
        if (!chartContent) {
            console.error(`[${this.container.id}] Stacked bar chart content area not found.`);
            return;
        }
        this.setupSvgWithDimensions(chartContent);

        // Handle empty data case
        if (!this.chartData || this.chartData.length === 0) {
            console.log(`[${this.container.id}] No data to render stacked bar chart, skipping chart rendering`);
            this.g.append('text')
                .attr('x', (this.width - this.margin.left - this.margin.right) / 2)
                .attr('y', (this.height - this.margin.top - this.margin.bottom) / 2)
                .attr('text-anchor', 'middle')
                .style('font-size', '14px')
                .style('fill', '#666')
                .text('No data available');
            return;
        }

        const chartWidth = this.width - this.margin.left - this.margin.right;
        const chartHeight = this.height - this.margin.top - this.margin.bottom;

        const keys = [...new Set(this.chartData.map(d => d[this.stackColumnName]))];
        const groupKeys = [...new Set(this.chartData.map(d => d[this.xColumnName]))];

        const groupedData = d3.group(this.chartData, (d: any) => d[this.xColumnName]);
        const processedData = Array.from(groupedData, ([key, values]) => {
            const obj: any = { [this.xColumnName]: key };
            keys.forEach(k => obj[k] = 0);
            values.forEach((v: any) => {
                obj[v[this.stackColumnName]] = v[this.yColumnName];
            });
            return obj;
        });

        const stack = d3.stack().keys(keys);
        const series = stack(processedData);

        this.xScale = d3.scaleBand()
            .domain(groupKeys)
            .range([0, chartWidth])
            .padding(0.1);

        this.yScale = d3.scaleLinear()
            .domain([0, d3.max(series, (d: any) => d3.max(d, (d: any) => +d[1])) || 0])
            .range([chartHeight, 0])
            .nice();

        this.colorScale = d3.scaleOrdinal(d3.schemeCategory10).domain(keys);

        const brush = d3.brush()
            .extent([[0, 0], [chartWidth, chartHeight]])
            .on('end', (event: { selection: any; }) => {
                console.log(`[${this.container.id}] Stacked bar chart brush event ended.`);
                if (!event.selection) {
                    console.log(`[${this.container.id}] No selection, clearing filters.`);
                    this.g.selectAll('rect').style('opacity', 1.0);
                    if (this.filterManager) {
                        this.filterManager.clearFiltersForChart(this);
                    }
                    return;
                }
                const [[x0, y0], [x1, y1]] = event.selection;

                // Get selected data for filtering
                const selectedData: any[] = [];
                this.g.selectAll('rect').each((d: any) => {
                    // Add null checks to prevent errors
                    if (!d || !d.data || !d.data[this.xColumnName]) {
                        return;
                    }
                    const barX = this.xScale(d.data[this.xColumnName]);
                    const barY = this.yScale(d[1]);
                    const barWidth = this.xScale.bandwidth();
                    const barHeight = this.yScale(d[0]) - this.yScale(d[1]);
                    const isBrushed = x0 < barX + barWidth && x1 > barX && y0 < barY + barHeight && y1 > barY;
                    if (isBrushed) {
                        selectedData.push(d);
                    }
                });

                // Apply visual selection
                this.g.selectAll('rect').style('opacity', (d: any) => {
                    // Add null checks to prevent errors
                    if (!d || !d.data || !d.data[this.xColumnName]) {
                        return 0.2;
                    }
                    const barX = this.xScale(d.data[this.xColumnName]);
                    const barY = this.yScale(d[1]);
                    const barWidth = this.xScale.bandwidth();
                    const barHeight = this.yScale(d[0]) - this.yScale(d[1]);
                    const isBrushed = x0 < barX + barWidth && x1 > barX && y0 < barY + barHeight && y1 > barY;
                    return isBrushed ? 1.0 : 0.2;
                });

                // Apply filter through FilterManager if we have selected data
                if (this.filterManager && selectedData.length > 0) {
                    console.log(`[${this.container.id}] Applying stacked bar chart brush filter with ${selectedData.length} selected bars`);

                    // Get unique values for x-axis from selected data
                    const xValues = [...new Set(selectedData.map(d => d.data[this.xColumnName]))];

                    // Check if x-axis is numeric or categorical
                    const sampleValue = xValues.length > 0 ? xValues[0] : null;
                    const isNumeric = sampleValue !== null && !isNaN(Number(sampleValue));

                    const filters: any = {};

                    if (isNumeric) {
                        // For numeric x-axis, convert brush positions to data values
                        const numericValues = xValues.map(v => Number(v)).sort((a, b) => a - b);
                        const minValue = numericValues[0];
                        const maxValue = numericValues[numericValues.length - 1];

                        filters[this.xColumnName] = {
                            type: 'AND',
                            conditions: [
                                { type: 'RANGE', operator: '>=', value: minValue },
                                { type: 'RANGE', operator: '<=', value: maxValue }
                            ],
                            raw: `${this.xColumnName}: ${minValue.toFixed(2)} - ${maxValue.toFixed(2)}`
                        };
                    } else {
                        // For categorical x-axis, use IN filter
                        filters[this.xColumnName] = {
                            type: 'IN',
                            values: new Set(xValues),
                            raw: `${this.xColumnName} IN (${xValues.join(', ')})`
                        };
                    }

                    this.filterManager.setFilters(filters, this);
                }
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

                // Apply filter through FilterManager if we have selected data
                if (this.filterManager && !isSelected) {
                    console.log(`[${this.container.id}] Applying stacked bar chart filter for clicked bar`);

                    // Get the data associated with this bar
                    const xValue = d.data[this.xColumnName];
                    const stackValue = (d3.select(event.currentTarget.parentNode).datum() as any).key;

                    const filters: any = {};

                    // Add X-axis filter
                    filters[this.xColumnName] = {
                        type: 'IN',
                        values: new Set([xValue]),
                        raw: `${this.xColumnName} = ${xValue}`
                    };

                    // Add stack filter
                    filters[this.stackColumnName] = {
                        type: 'IN',
                        values: new Set([stackValue]),
                        raw: `${this.stackColumnName} = ${stackValue}`
                    };

                    this.filterManager.setFilters(filters, this);
                } else if (this.filterManager && isSelected) {
                    // Clear filters when deselecting
                    this.filterManager.clearFiltersForChart(this);
                }
            });

        const tooltipManager = TooltipManager.getInstance();
        tooltipManager.addTooltip(bars, (d: any, el: any) => {
          const parentNode = el.parentNode;
          const stackValue = d3.select(parentNode).datum() as any;
          return `
            <strong>${this.xColumnName}:</strong> ${d.data[this.xColumnName]}<br>
            <strong>${this.stackColumnName}:</strong> ${stackValue.key}<br>
            <strong>${this.yColumnName}:</strong> ${d.data[stackValue.key]}
          `;
        });

        this.g.append('g')
            .attr('transform', `translate(0,${chartHeight})`)
            .call(d3.axisBottom(this.xScale))
            .selectAll('text')
            .style('text-anchor', 'end')
            .attr('dx', '-.8em')
            .attr('dy', '.15em')
            .attr('transform', 'rotate(-45)')
            .text((d: any, i: number) => {
              if (i % 2 !== 0) return '';
              return truncate(String(d), 10);
            });

        this.g.append('g')
            .call(d3.axisLeft(this.yScale).tickFormat((d: any) => truncate(String(d), 10)));

        this.g.append('g')
            .attr('class', 'grid')
            .call(d3.axisLeft(this.yScale).tickSize(-chartWidth).tickFormat('' as any))
            .call((g: any) => g.select('.domain').remove())
            .selectAll('line')
            .attr('stroke', 'rgba(0,0,0,0.1)');

        this.renderLegend();
    }

    redrawWithoutRefetch() {
        this.render();
    }

    destroy() {}

    renderLegend() {
        const chartContent = this.container.querySelector('.chart-content');
        if (!chartContent) return;
        d3.select(chartContent).select('.chart-legend').remove();

        const legendContainer = d3.select(chartContent).append('div').attr('class', 'chart-legend');

        const maxLegendItems = 10;
        const legendData = this.colorScale.domain();
        const truncated = legendData.length > maxLegendItems;
        const data = truncated ? legendData.slice(0, maxLegendItems) : legendData;

        const legendItems = legendContainer.selectAll('.legend-item')
            .data(data)
            .enter()
            .append('div')
            .attr('class', 'legend-item')
            .on('click', (_event: MouseEvent, d: any) => {
                if (this.filterManager) {
                    this.filterManager.setFilters(
                        {[this.stackColumnName]: {type: "IN", values: new Set([d])}},
                        this
                    );
                }
            });

        legendItems.append('span')
            .attr('class', 'legend-swatch')
            .style('background-color', (d: any) => this.colorScale(d));

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

export class D3StackedBarChartComponent implements m.Component<{
    dataProvider: any,
    xColumnName: string,
    yColumnName: string,
    stackColumnName: string,
    filterManager?: any,
}> {
    chart: D3StackedBarChart|null = null;
    dom: HTMLElement | null = null;

    oncreate(vnode: m.VnodeDOM<{
        dataProvider: any,
        xColumnName: string,
        yColumnName: string,
        stackColumnName: string,
        filterManager?: any,
    }>) {
        this.dom = vnode.dom as HTMLElement;
        this.dom.id = `d3-stacked-bar-chart-${Math.random().toString(36).substr(2, 9)}`;

        this.chart = new D3StackedBarChart(this.dom, vnode.attrs);
        if (vnode.attrs.filterManager) {
            vnode.attrs.filterManager.subscribe(this);
        }
        this.chart.render();
        ComponentManager.register(this);
        ChartManager.register(this);
    }

    async refreshFilteredData() {
        console.log(`[${this.chart?.container.id}] Stacked bar chart refreshFilteredData called`);
        if (this.chart) {
            await this.chart.render();
            m.redraw();
        }
    }

    // Required method for FilterManager integration
    setIsFilterSource(isSource: boolean) {
        console.log(`[${this.chart?.container.id}] Stacked bar chart setIsFilterSource called with:`, isSource);
        if (this.chart) {
            this.chart.isFilterSource = isSource;
            // Update visual styling to indicate filter source
            if (this.dom) {
                const header = this.dom.querySelector('.chart-header');
                if (header) {
                    header.classList.toggle('filter-source', isSource);
                }
            }

            // Auto-refresh when becoming filter source if flag is enabled
            if (isSource && (window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER === true) {
                this.refreshFilteredData();
            }
        }
    }

    onremove(vnode: any) {
        if (vnode.attrs.filterManager) {
            vnode.attrs.filterManager.unsubscribe(this);
        }
        if (this.chart) {
          this.chart.destroy();
        }
        ChartManager.unregister(this);
      }

    view() {
        return m(
            '.chart-container',
            {style: {width: '500px', height: '400px'}},
            m('.chart-header',
              {
                onclick: (event: MouseEvent) => {
                  event.stopPropagation();
                  if (this.chart?.filterManager) {
                    this.chart.filterManager.clearFiltersForChart(this.chart);
                  }
                },
              },
              m('h4.chart-title', 'Stacked Bar Chart'),
              m('.chart-actions',
                m('button.chart-action-btn', {title: 'Duplicate chart', onclick: () => ChartManager.duplicateChart(this.dom)}, '⧉'),
                m('button.chart-close-btn', {title: 'Remove Chart', onclick: () => ChartManager.removeChart(this.dom)}, '×'),
              ),
            ),
            m('.chart-content', {style: {'overflow-x': 'auto', 'overflow-y': 'hidden'}}),
            m('.resize-handle', {
              onmousedown: (e: MouseEvent) => {
                if (this.dom) {
                  ResizeManager.getInstance().onMouseDown(
                      e, this.dom, this.dom);
                }
              },
            }),
          );
    }
}

class D3AreaChart {
    container: HTMLElement;
    width: number = 600;
    height: number = 400;
    margin = { top: 10, right: 180, bottom: 150, left: 60 };
    svg: any;
    g: any;
    chartData: any[] = [];
    xColumnName: string;
    yColumnName: string;
    stackColumnName: string;
    aggregationFunction: string;
    dataProvider: any;
    xScale: any;
    yScale: any;
    colorScale: any;
    clipPathManager!: ClipPathManager;
    filterManager: any;
    isFilterSource: boolean = false;

    constructor(container: HTMLElement, attrs: any) {
        this.container = container;
        this.dataProvider = attrs.dataProvider;
        this.xColumnName = attrs.xColumnName;
        this.yColumnName = attrs.yColumnName;
        this.stackColumnName = attrs.stackColumnName;
        this.aggregationFunction = attrs.aggregationFunction;
        this.filterManager = attrs.filterManager;
        if (this.aggregationFunction) {
            console.log('Aggregation function', this.aggregationFunction, 'is not yet implemented for Area charts.');
        }
    }

    getContainerDimensions() {
        const rect = this.container.getBoundingClientRect();
        return {
            width: rect.width || 600,
            height: rect.height || 400,
        };
    }

    async loadData() {
        console.log(`[${this.container.id}] Loading data for area chart...`);
        console.log(`[${this.container.id}] FilterManager active filters:`, this.filterManager ? this.filterManager.getFilters() : 'No FilterManager');

        const querySpec = {
          filters: this.filterManager ? this.filterManager.getFiltersForQuery() : [],
        };
        const result = await this.dataProvider.query(querySpec);
        this.chartData = result.data;

        console.log(`[${this.container.id}] Data loaded for area chart:`, this.chartData.length, 'rows');
    }

    setupSvgWithDimensions(chartContent: any) {
        if (!chartContent) return null;
        d3.select(chartContent).selectAll("*").remove();
        const containerDimensions = this.getContainerDimensions();
        this.width = containerDimensions.width;
        this.height = containerDimensions.height;
        this.svg = d3.select(chartContent)
            .append('svg')
            .attr('width', this.width)
            .attr('height', this.height);
        this.g = this.svg.append('g').attr('transform', `translate(${this.margin.left},${this.margin.top})`);
        this.clipPathManager = new ClipPathManager(this.svg);
        return this.svg;
    }

    async render() {
        console.log(`[${this.container.id}] Rendering area chart...`);
        await this.loadData();
        const chartContent = this.container.querySelector('.chart-content');
        if (!chartContent) {
            console.error(`[${this.container.id}] Area chart content area not found.`);
            return;
        }
        this.setupSvgWithDimensions(chartContent);

        // Handle empty data case
        if (!this.chartData || this.chartData.length === 0) {
            console.log(`[${this.container.id}] No data to render area chart, skipping chart rendering`);
            this.g.append('text')
                .attr('x', (this.width - this.margin.left - this.margin.right) / 2)
                .attr('y', (this.height - this.margin.top - this.margin.bottom) / 2)
                .attr('text-anchor', 'middle')
                .style('font-size', '14px')
                .style('fill', '#666')
                .text('No data available');
            return;
        }

        const chartWidth = this.width - this.margin.left - this.margin.right;
        const chartHeight = this.height - this.margin.top - this.margin.bottom;

        const keys = [...new Set(this.chartData.map(d => d[this.stackColumnName]))];
        const groupKeys = [...new Set(this.chartData.map(d => d[this.xColumnName]))];

        const groupedData = d3.group(this.chartData, (d: any) => d[this.xColumnName]);
        const processedData = Array.from(groupedData, ([key, values]) => {
            const obj: any = { [this.xColumnName]: key };
            keys.forEach(k => obj[k] = 0);
            values.forEach((v: any) => {
                obj[v[this.stackColumnName]] = v[this.yColumnName];
            });
            return obj;
        });

        const stack = d3.stack().keys(keys);
        const series = stack(processedData);

        this.xScale = d3.scaleBand()
            .domain(groupKeys)
            .range([0, chartWidth])
            .padding(0.1);

        this.yScale = d3.scaleLinear()
            .domain([0, d3.max(series, (d: any) => d3.max(d, (d: any) => +d[1])) || 0])
            .range([chartHeight, 0])
            .nice();

        this.colorScale = d3.scaleOrdinal(d3.schemeCategory10).domain(keys);

        const brush = d3.brushX()
            .extent([[0, 0], [chartWidth, chartHeight]])
            .on('end', (event: { selection: any; }) => {
                this.handleBrush(event.selection, series);
            });

        this.g.append('g')
            .attr('class', 'brush')
            .call(brush);

        this.drawAreas(this.g, series, 1.0);

        this.g.append('g')
            .attr('transform', `translate(0,${chartHeight})`)
            .call(d3.axisBottom(this.xScale))
            .selectAll('text')
            .style('text-anchor', 'end')
            .attr('dx', '-.8em')
            .attr('dy', '.15em')
            .attr('transform', 'rotate(-45)')
            .text((d: any, i: number) => {
              if (i % 2 !== 0) return '';
              return truncate(String(d), 10);
            });

        this.g.append('g')
            .call(d3.axisLeft(this.yScale).tickFormat((d: any) => truncate(String(d), 10)));

        this.g.append('g')
            .attr('class', 'grid')
            .call(d3.axisLeft(this.yScale).tickSize(-chartWidth).tickFormat('' as any))
            .call((g: any) => g.select('.domain').remove())
            .selectAll('line')
            .attr('stroke', 'rgba(0,0,0,0.1)');

        this.g.append('g')
            .attr('class', 'grid')
            .call(d3.axisLeft(this.yScale).tickSize(-chartWidth).tickFormat('' as any))
            .call((g: any) => g.select('.domain').remove())
            .selectAll('line')
            .attr('stroke', 'rgba(0,0,0,0.1)');

        this.renderLegend();
    }

    handleBrush(selection: any, series: any) {
        console.log(`[${this.container.id}] Area chart brush event handled.`);
        this.g.selectAll('.area-dimmed').remove();
        this.g.selectAll('.area-highlight').remove();
        this.clipPathManager.removeAllClips();

        if (!selection) {
            console.log(`[${this.container.id}] No selection, clearing filters.`);
            this.g.selectAll('.area').style('opacity', 1);
            if (this.filterManager) {
                this.filterManager.clearFiltersForChart(this);
            }
            return;
        }

        this.g.selectAll('.area').style('opacity', 0);
        this.drawAreas(this.g.append('g').attr('class', 'area-dimmed'), series, 0.2);

        const [x0, x1] = selection;
        const clipUrl = this.clipPathManager.createRectClip(x0, 0, x1 - x0, this.height);
        const highlightGroup = this.g.append('g').attr('class', 'area-highlight').attr('clip-path', clipUrl);
        this.drawAreas(highlightGroup, series, 1.0);

        // Apply filter through FilterManager if we have a selection
        if (this.filterManager && selection) {
            console.log(`[${this.container.id}] Applying area chart filter for brush selection`);

            // Check if x-axis is numeric or categorical
            const sampleValue = this.chartData.length > 0 ? this.chartData[0][this.xColumnName] : null;
            const isNumeric = sampleValue !== null && !isNaN(Number(sampleValue));

            if (isNumeric) {
                // For numeric x-axis, convert brush positions to data values
                const minX = Math.min(...this.xScale.domain().map((d: any) => Number(d)));
                const maxX = Math.max(...this.xScale.domain().map((d: any) => Number(d)));
                const xRange = maxX - minX;
                const chartWidth = this.width - this.margin.left - this.margin.right;

                const minValue = minX + (x0 / chartWidth) * xRange;
                const maxValue = minX + (x1 / chartWidth) * xRange;

                const filters: any = {};
                filters[this.xColumnName] = {
                    type: 'AND',
                    conditions: [
                        { type: 'RANGE', operator: '>=', value: minValue },
                        { type: 'RANGE', operator: '<=', value: maxValue }
                    ],
                    raw: `${this.xColumnName}: ${minValue.toFixed(2)} - ${maxValue.toFixed(2)}`
                };

                this.filterManager.setFilters(filters, this);
            } else {
                // For categorical x-axis, get the categories that fall within the brush selection
                const selectedCategories: string[] = [];
                this.xScale.domain().forEach((category: string) => {
                    const categoryPos = this.xScale(category)! + this.xScale.bandwidth() / 2;
                    if (categoryPos >= x0 && categoryPos <= x1) {
                        selectedCategories.push(category);
                    }
                });

                if (selectedCategories.length > 0) {
                    const filters: any = {};
                    filters[this.xColumnName] = {
                        type: 'IN',
                        values: new Set(selectedCategories),
                        raw: `${this.xColumnName} IN (${selectedCategories.join(', ')})`
                    };

                    this.filterManager.setFilters(filters, this);
                }
            }
        }
    }

    drawAreas(container: any, series: any, opacity: number) {
        const area = d3.area()
            .x((d: any) => this.xScale(d.data[this.xColumnName]) + this.xScale.bandwidth() / 2)
            .y0((d: any) => this.yScale(d[0]))
            .y1((d: any) => this.yScale(d[1]));

        container.selectAll('.area')
            .data(series)
            .enter().append('path')
            .attr('class', 'area')
            .attr('d', area)
            .style('fill', (d: any) => this.colorScale(d.key))
            .style('opacity', opacity);
    }

    redrawWithoutRefetch() {
        this.render();
    }

    destroy() {}

    renderLegend() {
        const chartContent = this.container.querySelector('.chart-content');
        if (!chartContent) return;
        d3.select(chartContent).select('.chart-legend').remove();

        const legendContainer = d3.select(chartContent).append('div').attr('class', 'chart-legend');

        const maxLegendItems = 10;
        const legendData = this.colorScale.domain();
        const truncated = legendData.length > maxLegendItems;
        const data = truncated ? legendData.slice(0, maxLegendItems) : legendData;

        const legendItems = legendContainer.selectAll('.legend-item')
            .data(data)
            .enter()
            .append('div')
            .attr('class', 'legend-item')
            .on('click', (_event: MouseEvent, d: any) => {
                if (this.filterManager) {
                    this.filterManager.setFilters(
                        {[this.stackColumnName]: {type: "IN", values: new Set([d])}},
                        this
                    );
                }
            });

        legendItems.append('span')
            .attr('class', 'legend-swatch')
            .style('background-color', (d: any) => this.colorScale(d));

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

export class D3AreaChartComponent implements m.Component<{
    dataProvider: any,
    xColumnName: string,
    yColumnName: string,
    stackColumnName: string,
    filterManager?: any,
}> {
    chart: D3AreaChart|null = null;
    dom: HTMLElement | null = null;

    oncreate(vnode: m.VnodeDOM<{
        dataProvider: any,
        xColumnName: string,
        yColumnName: string,
        stackColumnName: string,
        filterManager?: any,
    }>) {
        this.dom = vnode.dom as HTMLElement;
        this.dom.id = `d3-area-chart-${Math.random().toString(36).substr(2, 9)}`;

        this.chart = new D3AreaChart(this.dom, {
            ...vnode.attrs,
            filterManager: vnode.attrs.filterManager // Ensure FilterManager is passed to D3 class
        });
        if (vnode.attrs.filterManager) {
            vnode.attrs.filterManager.subscribe(this);
        }
        this.chart.render();
        ComponentManager.register(this);
        ChartManager.register(this);
    }

    async refreshFilteredData() {
        console.log(`[${this.chart?.container.id}] Area chart refreshFilteredData called`);
        if (this.chart) {
            await this.chart.render();
            m.redraw();
        }
    }

    // Required method for FilterManager integration
    setIsFilterSource(isSource: boolean) {
        console.log(`[${this.chart?.container.id}] Area chart setIsFilterSource called with:`, isSource);
        if (this.chart) {
            this.chart.isFilterSource = isSource;
            // Update visual styling to indicate filter source
            if (this.dom) {
                const header = this.dom.querySelector('.chart-header');
                if (header) {
                    header.classList.toggle('filter-source', isSource);
                }
            }

            // Auto-refresh when becoming filter source if flag is enabled
            if (isSource && (window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER === true) {
                this.refreshFilteredData();
            }
        }
    }

    onremove(vnode: any) {
        if (vnode.attrs.filterManager) {
            vnode.attrs.filterManager.unsubscribe(this);
        }
        if (this.chart) {
          this.chart.destroy();
        }
        ChartManager.unregister(this);
      }

    view() {
        return m(
            '.chart-container',
            {style: {width: '500px', height: '400px'}},
            m('.chart-header',
              {
                onclick: (event: MouseEvent) => {
                  event.stopPropagation();
                  if (this.chart?.filterManager) {
                    this.chart.filterManager.clearFiltersForChart(this.chart);
                  }
                },
              },
              m('h4.chart-title', 'Area Chart'),
              m('.chart-actions',
                m('button.chart-action-btn', {title: 'Duplicate chart', onclick: () => ChartManager.duplicateChart(this.dom)}, '⧉'),
                m('button.chart-close-btn', {title: 'Remove Chart', onclick: () => ChartManager.removeChart(this.dom)}, '×'),
              ),
            ),
            m('.chart-content', {style: {'overflow-x': 'auto', 'overflow-y': 'hidden'}}),
            m('.resize-handle', {
              onmousedown: (e: MouseEvent) => {
                if (this.dom) {
                  ResizeManager.getInstance().onMouseDown(
                      e, this.dom, this.dom);
                }
              },
            }),
        );
    }
}

export class FilterManager {
  private filters: Map<string, any[]> = new Map(); // Stack-based filters
  private subscribers: Set<any> = new Set();
  private static nextId = 0;
  private id: number;

  constructor() {
    this.id = FilterManager.nextId++;
    console.log(`[FilterManager ${this.id}] Created`);
  }

  setDataProvider(_dataProvider: any) {
    // No-op. FilterManager no longer holds a reference to the data provider.
    console.log(`[FilterManager ${this.id}] setDataProvider called`);
  }

  subscribe(component: any) {
    this.subscribers.add(component);
    const chartId = component?.chart?.container?.id || component?.container?.id || 'unknown';
    console.log(`[FilterManager ${this.id}] Subscribed chart: ${chartId}`);
  }

  unsubscribe(component: any) {
    this.subscribers.delete(component);
    const chartId = component?.chart?.container?.id || component?.container?.id || 'unknown';
    console.log(`[FilterManager ${this.id}] Unsubscribed chart: ${chartId}`);
  }

async setFilters(filterMap: Record<string, any>, sourceChart: any, refreshSource: boolean = false): Promise<void> {
  const sourceChartId = sourceChart?.chart?.container?.id || sourceChart?.container?.id;
  console.log(`[FilterManager ${this.id}] setFilters called from chart: ${sourceChartId || 'unknown'}`);
  console.log(`[FilterManager ${this.id}] filterMap:`, filterMap);
  console.log(`[FilterManager ${this.id}] refreshSource:`, refreshSource);

  // Check the global flag to determine if source should refresh
  const globalUpdateSourceFlag = (window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER;
  console.log(`[FilterManager ${this.id}] Global update source flag:`, globalUpdateSourceFlag);

  for (const [column, filter] of Object.entries(filterMap)) {
    if (!column || column === "null" || column === "undefined") {
      console.log(`[FilterManager ${this.id}] Skipping invalid column: ${column}`);
      continue;
    }

    const stack = this.filters.get(column) || [];
    const newStack = [...stack];

    if (filter) {
      const top = newStack.length > 0 ? newStack[newStack.length - 1] : null;
      if (top && top.sourceChart === sourceChart) {
        // Replace existing filter from same chart
        newStack[newStack.length - 1] = { filter, sourceChart };
        console.log(`[FilterManager ${this.id}] Replaced filter for column ${column}`);
      } else {
        // Add new filter to stack
        newStack.push({ filter, sourceChart });
        console.log(`[FilterManager ${this.id}] Added new filter for column ${column}`);
      }
    }

    if (newStack.length > 0) {
      this.filters.set(column, newStack);
    } else {
      this.filters.delete(column);
    }
  }

  console.log(`[FilterManager ${this.id}] Current active filters:`, this.getFilters());

  this.updateAllChartsFilterSourceState();
  this.updateURLWithFilters();

  const operationId = Symbol('filter-operation');
  console.log(`[FilterManager ${this.id}] Starting filter operation with ID:`, operationId.toString());

  try {
    const promises: Promise<void>[] = [];
    this.subscribers.forEach((chart) => {
      const sourceChartId = sourceChart?.chart?.container?.id || sourceChart?.container?.id;
      const currentChartId = chart?.chart?.container?.id || chart?.container?.id;
      const isSourceChart = sourceChartId && currentChartId && sourceChartId === currentChartId;

      const shouldRefreshSource = globalUpdateSourceFlag || refreshSource;
      const shouldRefreshThisChart = !isSourceChart || (isSourceChart && shouldRefreshSource);

      if (shouldRefreshThisChart) {
        console.log(`[FilterManager ${this.id}] Refreshing chart: ${currentChartId} (isSource: ${isSourceChart})`);
        chart._currentFilterOperation = operationId;
        promises.push(chart.refreshFilteredData());
      } else {
        console.log(`[FilterManager ${this.id}] Skipping source chart refresh (global flag disabled): ${chart.chart?.container?.id || 'unknown'}`);
      }
    });

    console.log(`[FilterManager ${this.id}] Waiting for ${promises.length} charts to refresh`);
    await Promise.all(promises);
    console.log(`[FilterManager ${this.id}] All charts refreshed successfully`);
  } catch (error) {
    console.error(`[FilterManager ${this.id}] Error during chart refresh:`, error);
  } finally {
    // Clear operation tracking
    this.subscribers.forEach((chart) => {
      if (chart._currentFilterOperation === operationId) {
        delete chart._currentFilterOperation;
      }
    });
    console.log(`[FilterManager ${this.id}] Filter operation completed`);
  }
}

  clearAllFilters() {
    this.filters.clear();
    console.log(`[FilterManager ${this.id}] All filters cleared`);
  }

  async clearFiltersForChart(
    sourceChart: any,
    filterMap?: Record<string, any>,
    forceRefresh: boolean = false): Promise<void> {
    const sourceChartId =
      sourceChart?.chart?.container?.id || sourceChart?.container?.id;
    console.log(
      `[FilterManager ${this.id}] clearFiltersForChart called for chart: ${
        sourceChartId || 'unknown'}`,
    );

    let filtersRemoved = false;
    const isDataTable = sourceChart instanceof DataTableComponent;

    if (filterMap && !isDataTable) {
      for (const [column, filter] of Object.entries(filterMap)) {
        if (this.filters.has(column)) {
          const stack = this.filters.get(column)!;
          const newStack = stack.filter(
            (f) => f.sourceChart !== sourceChart || f.filter !== filter,
          );
          if (newStack.length !== stack.length) {
            filtersRemoved = true;
            if (newStack.length > 0) {
              this.filters.set(column, newStack);
            } else {
              this.filters.delete(column);
            }
          }
        }
      }
    } else {
      for (const [column, stack] of this.filters.entries()) {
        let newStack = stack;
        if (isDataTable) {
          newStack.pop();
        } else {
          newStack = stack.filter((f) => f.sourceChart !== sourceChart);
        }

        if (newStack.length !== stack.length) {
          filtersRemoved = true;
          console.log(`[FilterManager ${this.id}] Removed filters for column: ${column}`);
          if (newStack.length > 0) {
            this.filters.set(column, newStack);
          } else {
            this.filters.delete(column);
          }
        }
      }
    }

    if (filtersRemoved || forceRefresh) {
      console.log(`[FilterManager ${this.id}] Filters were removed, updating all charts`);
      this.notifySubscribers();
    } else {
      console.log(`[FilterManager ${this.id}] No filters were removed for this chart`);
    }
  }

  async notifySubscribers() {
    this.updateAllChartsFilterSourceState();
    this.updateURLWithFilters();

    const promises: Promise<void>[] = [];
    this.subscribers.forEach((chart) => {
      const chartId = chart?.chart?.container?.id || chart?.container?.id;
      console.log(
        `[FilterManager ${this.id}] Refreshing chart after clear: ${chartId || 'unknown'}`,
      );
      promises.push(chart.refreshFilteredData());
    });

    try {
      await Promise.all(promises);
      console.log(`[FilterManager ${this.id}] All charts refreshed after clear`);
    } catch (error) {
      console.error(
        `[FilterManager ${this.id}] Error during chart refresh after clear:`,
        error,
      );
    }
  }

  updateAllChartsFilterSourceState(): void {
    console.log(`[FilterManager ${this.id}] updateAllChartsFilterSourceState called`);

    const sourceChartIds = new Set();
    for (const stack of this.filters.values()) {
      if (stack.length > 0) {
        const top = stack[stack.length - 1];
        if (top.sourceChart) {
          const sourceChartId = top.sourceChart?.chart?.container?.id || top.sourceChart?.container?.id;
          if (sourceChartId) {
            sourceChartIds.add(sourceChartId);
          }
        }
      }
    }

    this.subscribers.forEach((chart) => {
      if (typeof chart.setIsFilterSource === "function") {
        const currentChartId = chart?.chart?.container?.id || chart?.container?.id;
        const isSource = currentChartId && sourceChartIds.has(currentChartId);
        
        // Special handling for DataTableComponent to avoid incorrect highlighting
        if (chart instanceof DataTableComponent) {
            chart.setIsFilterSource(isSource);
        } else {
            // For other charts, only set them as a source if they are not a DataTable
            const topFilter = this.filters.values().next().value?.slice(-1)[0];
            if (topFilter && topFilter.sourceChart instanceof DataTableComponent) {
                chart.setIsFilterSource(false);
            } else {
                chart.setIsFilterSource(isSource);
            }
        }
      }
    });
  }

  updateURLWithFilters() {
    // Update URL with current filter state
    console.log(`[FilterManager ${this.id}] updateURLWithFilters called`);
    const currentFilters = this.getFilters();
    console.log(`[FilterManager ${this.id}] Current filters for URL:`, currentFilters);
    // This method can be implemented to sync filters with URL if needed
  }

  getFilters(): Record<string, any> {
    const activeFilters: Record<string, any> = {};
    for (const [column, stack] of this.filters.entries()) {
      if (stack.length > 0) {
        activeFilters[column] = stack[stack.length - 1].filter;
      }
    }
    return activeFilters;
  }

  getFiltersForQuery(): any[] {
    const filtersForQuery: any[] = [];
    const activeFilters = this.getFilters();
    for (const column of Object.keys(activeFilters)) {
      const filter = activeFilters[column];
      if (filter.type === 'AND' && Array.isArray(filter.conditions)) {
        for (const condition of filter.conditions) {
          filtersForQuery.push(this.transformFilter(column, condition));
        }
      } else {
        filtersForQuery.push(this.transformFilter(column, filter));
      }
    }
    return filtersForQuery;
  }


  transformFilter(column: string, filter: any) {
    return {
      column,
      type: filter.type,
      operator: filter.operator,
      value: filter.values ? Array.from(filter.values) : filter.value,
      negate: filter.negate || false
    };
  }
}

/**
 * Utilities for converting between internal filter format and DataGrid FilterDefinition format
 */
class FilterConverter {
  /**
   * Converts internal filter format to DataGrid FilterDefinition array
   */
  static toFilterDefinitions(filterMap: Record<string, any>): FilterDefinition[] {
    const filterDefs: FilterDefinition[] = [];
    
    for (const [column, filter] of Object.entries(filterMap)) {
      if (!filter || !filter.type) continue;
      
      filterDefs.push(...this.convertSingleFilter(column, filter));
    }
    
    return filterDefs;
  }
  
  private static convertSingleFilter(column: string, filter: any): FilterDefinition[] {
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
  
  private static convertInFilter(column: string, filter: any): FilterDefinition[] {
    // Convert each value in the set to a separate '=' filter
    return Array.from(filter.values).map(value => ({
      column,
      op: '=' as const,
      value: value as any
    }));
  }
  
  private static convertRangeFilter(column: string, filter: any): FilterDefinition[] {
    return [{
      column,
      op: filter.operator as any,
      value: filter.value
    }];
  }
  
  private static convertLikeFilter(column: string, filter: any): FilterDefinition[] {
    return [{
      column,
      op: 'glob' as const,
      value: `*${filter.value}*`
    }];
  }
  
  private static convertAndFilter(column: string, filter: any): FilterDefinition[] {
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
    filters: ReadonlyArray<FilterDefinition>,
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
    currentFilters: ReadonlyArray<FilterDefinition>,
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
    columnFilters: FilterDefinition[]
  ): any {
    if (columnFilters.length === 0) return null;
    
    // Multiple filters for same column -> convert to IN filter
    if (columnFilters.length > 1) {
      return this.convertToInFilter(column, columnFilters);
    }
    
    // Single filter
    return this.convertSingleColumnFilter(column, columnFilters[0]);
  }
  
  private static convertToInFilter(column: string, filters: FilterDefinition[]): any {
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
  
  private static convertSingleColumnFilter(_column: string, filter: FilterDefinition): any {
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

// Add the global window update source flag that's in the working version
const storedValue = localStorage.getItem('updateSourceOnFilter');
(window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER = storedValue === null ? true : storedValue === 'true';

(window as any).setUpdateSourceOnFilter = function(value: boolean) {
  (window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER = value;
  localStorage.setItem('updateSourceOnFilter', value.toString());
};

class TooltipManager {
  static instance: TooltipManager | null = null;
  private tooltip: any;

  static getInstance(): TooltipManager {
    if (!TooltipManager.instance) {
      TooltipManager.instance = new TooltipManager();
    }
    return TooltipManager.instance;
  }

  constructor() {
    if (TooltipManager.instance) {
      return TooltipManager.instance;
    }

    this.tooltip = this.createTooltipElement();
    TooltipManager.instance = this;
  }

  createTooltipElement(): any {
    let tooltip = d3.select("body").select<HTMLDivElement>(".d3-chart-tooltip");
    if (tooltip.empty()) {
      tooltip = d3
        .select("body")
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

function truncate(str: string, maxLength: number): string {
  if (str.length <= maxLength) {
    return str;
  }
  return str.slice(0, maxLength) + '…';
}

import {DataGrid} from '../components/widgets/data_grid/data_grid';
import {
  ColumnDefinition,
  DataGridDataSource,
  FilterDefinition,
} from '../components/widgets/data_grid/common';
import {DataGridAttrs} from '../components/widgets/data_grid/data_grid';

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
    ComponentManager.register(this);
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
            resize: 'vertical',
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
            onFiltersChanged: (filters: ReadonlyArray<FilterDefinition>) => {
              this.handleFiltersChanged(filters, vnode);
            },
            filters: vnode.attrs.filterManager ? FilterConverter.toFilterDefinitions(vnode.attrs.filterManager.getFilters()) : [],
          }),
        ),
        m('.resize-handle', {
          onmousedown: (e: MouseEvent) => {
            if (this.dom) {
              ResizeManager.getInstance().onMouseDown(e, this.dom, this.dom);
            }
          },
        }),
    );
  }
  
  private handleFiltersChanged(
    filters: ReadonlyArray<FilterDefinition>,
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
