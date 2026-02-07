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
import {App} from '../../../public/app';
import {renderDocSection, renderWidgetShowcase} from '../widgets_page_utils';
import {MemorySource, FilterStore, Chart} from '../../../widgets/charts/d3';
import {
  ChartType,
  AggregationFunction,
  LayoutMode,
  SortBy,
  SortDirection,
} from '../../../widgets/charts/d3/data/types';
import {ChartWidget} from '../../../widgets/d3_chart_widget';

// Generate sample data for demonstrations
function generateSampleData() {
  const categories = ['Alpha', 'Beta', 'Gamma', 'Delta', 'Epsilon', 'Zeta'];
  const types = ['Type A', 'Type B', 'Type C'];
  const data = [];

  for (let i = 0; i < 100; i++) {
    data.push({
      category: categories[Math.floor(Math.random() * categories.length)],
      value: Math.floor(Math.random() * 100) + 10,
      duration: Math.random() * 1000,
      count: Math.floor(Math.random() * 50) + 1,
      type: types[Math.floor(Math.random() * types.length)],
      x_coord: Math.random() * 100,
      y_coord: Math.random() * 100,
    });
  }

  return data;
}

// Create shared instances outside the render function
// so they persist across redraws
const sampleData = generateSampleData();
const dataSource = new MemorySource(sampleData);
const filterStore = new FilterStore();

// Create charts once
const barChart = new Chart(
  {
    type: ChartType.Bar,
    x: 'category',
    y: 'value',
    aggregation: AggregationFunction.Sum,
    sort: {by: SortBy.Y, direction: SortDirection.Desc},
  },
  dataSource,
  filterStore,
);

const groupedBarChart = new Chart(
  {
    type: ChartType.Bar,
    x: 'category',
    y: 'value',
    aggregation: AggregationFunction.Sum,
    groupBy: 'type',
    mode: LayoutMode.Grouped,
    sort: {by: SortBy.X, direction: SortDirection.Asc},
  },
  dataSource,
  filterStore,
);

const stackedBarChart = new Chart(
  {
    type: ChartType.Bar,
    x: 'category',
    y: 'value',
    aggregation: AggregationFunction.Sum,
    groupBy: 'type',
    mode: LayoutMode.Stacked,
    sort: {by: SortBy.Y, direction: SortDirection.Asc},
  },
  dataSource,
  filterStore,
);

const histogramChart = new Chart(
  {
    type: ChartType.Histogram,
    x: 'duration',
    bins: 15,
  },
  dataSource,
  filterStore,
);

const cdfChart = new Chart(
  {
    type: ChartType.Cdf,
    x: 'value',
  },
  dataSource,
  filterStore,
);

const cdfColoredChart = new Chart(
  {
    type: ChartType.Cdf,
    x: 'duration',
    colorBy: 'type',
  },
  dataSource,
  filterStore,
);

const scatterChart = new Chart(
  {
    type: ChartType.Scatter,
    x: 'x_coord',
    y: 'y_coord',
    showCorrelation: true,
  },
  dataSource,
  filterStore,
);

const scatterColoredChart = new Chart(
  {
    type: ChartType.Scatter,
    x: 'value',
    y: 'duration',
    colorBy: 'type',
    showCorrelation: false,
  },
  dataSource,
  filterStore,
);

const boxplotChart = new Chart(
  {
    type: ChartType.Boxplot,
    x: 'category',
    y: 'value',
  },
  dataSource,
  filterStore,
);

const heatmapChart = new Chart(
  {
    type: ChartType.Heatmap,
    x: 'category',
    y: 'type',
    value: 'value',
    aggregation: AggregationFunction.Avg,
  },
  dataSource,
  filterStore,
);

const lineChart = new Chart(
  {
    type: ChartType.Line,
    x: 'count',
    y: 'value',
    aggregation: AggregationFunction.Avg,
  },
  dataSource,
  filterStore,
);

const lineColoredChart = new Chart(
  {
    type: ChartType.Line,
    x: 'count',
    y: 'value',
    aggregation: AggregationFunction.Avg,
    colorBy: 'type',
  },
  dataSource,
  filterStore,
);

const donutChart = new Chart(
  {
    type: ChartType.Donut,
    category: 'category',
    value: 'value',
    aggregation: AggregationFunction.Sum,
  },
  dataSource,
  filterStore,
);

const violinChart = new Chart(
  {
    type: ChartType.Violin,
    x: 'category',
    y: 'value',
  },
  dataSource,
  filterStore,
);

export function renderD3Charts(_app: App): m.Children {
  // Use the shared instances created above
  return [
    m('h1', 'D3 Charts'),
    m('p', [
      'Interactive D3-based charts with cross-filtering support. ',
      'Click on chart elements to filter data across all charts. ',
      'The filter store is shared, so filtering in one chart affects all others.',
    ]),

    renderWidgetShowcase({
      renderWidget: ({updateSourceChart}) => {
        // Update the filter store when the option changes
        filterStore.setUpdateSourceChart(updateSourceChart);

        return m('.chart-demo-container', [
          m('p', [
            'When "updateSourceChart" is disabled, the chart that creates a filter will not reload its data.',
          ]),
        ]);
      },
      initialOpts: {
        updateSourceChart: filterStore.getUpdateSourceChart(),
      },
    }),

    renderDocSection(
      'Bar Chart (sorted by value, descending)',
      m('.chart-demo-container', [
        m('p', 'Aggregated bar chart with click-to-filter functionality.'),
        m(ChartWidget, {chart: barChart}),
      ]),
    ),

    renderDocSection(
      'Grouped Bar Chart (sorted by category, ascending)',
      m('.chart-demo-container', [
        m(
          'p',
          'Multi-series bar chart with bars grouped side-by-side. Each group represents a different category, allowing easy comparison across groups.',
        ),
        m(ChartWidget, {chart: groupedBarChart}),
      ]),
    ),

    renderDocSection(
      'Stacked Bar Chart (sorted by value, ascending)',
      m('.chart-demo-container', [
        m(
          'p',
          'Multi-series bar chart with bars stacked on top of each other. Shows both individual values and total sum for each category.',
        ),
        m(ChartWidget, {chart: stackedBarChart}),
      ]),
    ),

    renderDocSection(
      'Histogram',
      m('.chart-demo-container', [
        m('p', 'Distribution histogram with configurable bins.'),
        m(ChartWidget, {chart: histogramChart}),
      ]),
    ),

    renderDocSection(
      'CDF (Cumulative Distribution Function)',
      m('.chart-demo-container', [
        m('p', 'Single CDF showing cumulative probability distribution.'),
        m(ChartWidget, {chart: cdfChart}),
      ]),
    ),

    renderDocSection(
      'CDF with Color Grouping',
      m('.chart-demo-container', [
        m('p', 'Multiple CDFs grouped by category with interactive legend.'),
        m(ChartWidget, {chart: cdfColoredChart}),
      ]),
    ),

    renderDocSection(
      'Scatter Plot',
      m('.chart-demo-container', [
        m(
          'p',
          "Basic scatter plot with correlation line and Pearson's r coefficient (showCorrelation: true).",
        ),
        m(ChartWidget, {chart: scatterChart}),
      ]),
    ),

    renderDocSection(
      'Scatter Plot with Color Grouping',
      m('.chart-demo-container', [
        m(
          'p',
          'Scatter plot with points colored by category, correlation line hidden (showCorrelation: false).',
        ),
        m(ChartWidget, {chart: scatterColoredChart}),
      ]),
    ),

    renderDocSection(
      'Boxplot',
      m('.chart-demo-container', [
        m(
          'p',
          'Boxplot showing distribution statistics (min, Q1, median, Q3, max) and outliers for each category.',
        ),
        m(ChartWidget, {chart: boxplotChart}),
      ]),
    ),

    renderDocSection(
      'Heatmap',
      m('.chart-demo-container', [
        m(
          'p',
          'Heatmap showing aggregated values across two categorical dimensions with color intensity.',
        ),
        m(ChartWidget, {chart: heatmapChart}),
      ]),
    ),

    renderDocSection(
      'Line Chart',
      m('.chart-demo-container', [
        m('p', 'Aggregated line chart showing average values by count.'),
        m(ChartWidget, {chart: lineChart}),
      ]),
    ),

    renderDocSection(
      'Line Chart with Color Grouping',
      m('.chart-demo-container', [
        m(
          'p',
          'Multiple aggregated lines grouped by type with interactive legend, showing average values across count.',
        ),
        m(ChartWidget, {chart: lineColoredChart}),
      ]),
    ),

    renderDocSection(
      'Donut Chart',
      m('.chart-demo-container', [
        m(
          'p',
          'Donut chart with shift-click multi-select. Click slices to filter, shift-click to select multiple.',
        ),
        m(ChartWidget, {chart: donutChart}),
      ]),
    ),

    renderDocSection(
      'Violin Plot',
      m('.chart-demo-container', [
        m(
          'p',
          'Violin plot showing distribution density with quartiles and percentiles (P90, P95, P99).',
        ),
        m(ChartWidget, {chart: violinChart}),
      ]),
    ),

    renderDocSection(
      'Active Filters',
      m(
        '.filter-display',
        {
          style:
            'padding: 16px; background: var(--pf-color-background-secondary); border-radius: 4px;',
        },
        [
          m('h3', {style: 'margin-top: 0;'}, 'Current Filters'),
          m(FilterDisplay, {filterStore}),
        ],
      ),
    ),
  ];
}

interface FilterDisplayAttrs {
  filterStore: FilterStore;
}

const FilterDisplay: m.ClosureComponent<FilterDisplayAttrs> = () => {
  let unsub: (() => void) | undefined;

  return {
    oninit(vnode) {
      // Subscribe to filter changes and trigger redraw
      unsub = vnode.attrs.filterStore.subscribe(() => {
        m.redraw();
      });
    },

    onremove() {
      // Clean up subscription
      if (unsub) {
        unsub();
      }
    },

    view({attrs}) {
      const filterGroups = attrs.filterStore.getFilterGroups();

      if (filterGroups.length === 0) {
        return m(
          'p',
          {style: 'color: var(--pf-color-text-secondary);'},
          'No active filters',
        );
      }

      return [
        m(
          'ul',
          {style: 'list-style: none; padding: 0; margin: 0;'},
          filterGroups.map((group) =>
            m(
              'li',
              {
                key: group.id,
                style:
                  'padding: 12px; margin: 4px 0; background: var(--pf-color-background); border-radius: 4px; border-left: 4px solid var(--pf-color-primary);',
              },
              [
                m(
                  '.filter-group-header',
                  {
                    style:
                      'display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px;',
                  },
                  [
                    m(
                      'strong',
                      {style: 'color: var(--pf-color-text-primary);'},
                      group.label || group.id,
                    ),
                    m(
                      'button',
                      {
                        onclick: () =>
                          attrs.filterStore.clearFilterGroup(group.id, 'ui'),
                        style:
                          'background: var(--pf-color-error); color: white; border: none; padding: 4px 8px; border-radius: 3px; cursor: pointer; font-size: 14px;',
                        title: 'Remove this filter group',
                      },
                      '×',
                    ),
                  ],
                ),
                m(
                  '.filter-group-details',
                  {
                    style:
                      'font-size: 12px; color: var(--pf-color-text-secondary); padding-left: 8px;',
                  },
                  group.filters.map((filter, idx) =>
                    m('div', {key: idx}, [
                      m(
                        'code',
                        {
                          style:
                            'background: var(--pf-color-background-secondary); padding: 2px 4px; border-radius: 2px;',
                        },
                        `${filter.col} ${filter.op} ${JSON.stringify(filter.val)}`,
                      ),
                    ]),
                  ),
                ),
              ],
            ),
          ),
        ),
        m(
          'button',
          {
            onclick: () => attrs.filterStore.clearAll(),
            style:
              'margin-top: 8px; background: var(--pf-color-primary); color: white; border: none; padding: 8px 16px; border-radius: 4px; cursor: pointer;',
          },
          'Clear All Filters',
        ),
      ];
    },
  };
};
