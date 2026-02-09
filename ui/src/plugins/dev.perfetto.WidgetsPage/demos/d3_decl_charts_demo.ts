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
import {Filter} from '../../../components/widgets/datagrid/model';
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import {InMemoryDataSource} from '../../../components/widgets/datagrid/in_memory_data_source';
import {SchemaRegistry} from '../../../components/widgets/datagrid/datagrid_schema';
import {
  Histogram,
  InMemoryHistogramLoader,
  InMemoryBarLoader,
  CDFChart,
  InMemoryCDFLoader,
  ScatterPlot,
  InMemoryScatterLoader,
  SimpleBarChart as BarChart,
  GroupedBarChart,
  StackedBarChart,
  GroupedBarData,
} from '../../../components/charts/d3-decl';

const SCHEMA: SchemaRegistry = {
  sample: {
    id: {title: 'ID', columnType: 'quantitative'},
    category: {title: 'Category', columnType: 'text'},
    value: {title: 'Value', columnType: 'quantitative'},
    group: {title: 'Group', columnType: 'text'},
    x: {title: 'X', columnType: 'quantitative'},
    y: {title: 'Y', columnType: 'quantitative'},
  },
};

export function renderD3DeclCharts(_app: App): m.Children {
  return m(ChartsDemo);
}

class ChartsDemo implements m.ClassComponent {
  private filters: Filter[] = [];
  private dataSource: InMemoryDataSource;
  private histogramLoader: InMemoryHistogramLoader;
  private barLoader: InMemoryBarLoader;
  private groupedBarLoader: InMemoryBarLoader;
  private stackedBarLoader: InMemoryBarLoader;
  private cdfLoader: InMemoryCDFLoader;
  private groupedCDFLoader: InMemoryCDFLoader;
  private scatterLoader: InMemoryScatterLoader;
  private coloredScatterLoader: InMemoryScatterLoader;
  private showCorrelation = true;
  private showPercentiles = false;

  constructor() {
    const data = this.generateData();
    this.dataSource = new InMemoryDataSource(data);

    this.histogramLoader = new InMemoryHistogramLoader({
      data,
      valueCol: 'value',
    });

    this.cdfLoader = new InMemoryCDFLoader({
      data,
      valueCol: 'value',
    });

    this.barLoader = new InMemoryBarLoader({
      data,
      categoryCol: 'category',
      valueCol: 'value',
    });

    this.scatterLoader = new InMemoryScatterLoader({
      data,
      xCol: 'x',
      yCol: 'y',
    });

    this.coloredScatterLoader = new InMemoryScatterLoader({
      data,
      xCol: 'x',
      yCol: 'y',
      categoryCol: 'group',
    });

    this.groupedBarLoader = new InMemoryBarLoader({
      data,
      categoryCol: 'category',
      valueCol: 'value',
      groupCol: 'group',
    });

    this.stackedBarLoader = new InMemoryBarLoader({
      data,
      categoryCol: 'category',
      valueCol: 'value',
      groupCol: 'group',
    });

    this.groupedCDFLoader = new InMemoryCDFLoader({
      data,
      valueCol: 'value',
      groupCol: 'group',
    });
  }

  private generateData() {
    const categories = ['Alpha', 'Beta', 'Gamma', 'Delta', 'Epsilon'];
    const groups = ['Group A', 'Group B', 'Group C'];
    const data: Array<{
      id: number;
      category: string;
      value: number;
      x: number;
      y: number;
      group: string;
    }> = [];

    for (let i = 0; i < 1000; i++) {
      const category =
        categories[Math.floor(Math.random() * categories.length)];
      const group = groups[Math.floor(Math.random() * groups.length)];
      const value = Math.random() * 100;
      const x = Math.random() * 100;
      const noise = (Math.random() - 0.5) * 20;
      const y = 0.8 * x + 10 + noise;

      data.push({id: i + 1, category, value, x, y, group});
    }

    return data;
  }

  private updateFilters(filters: readonly Filter[]) {
    this.filters = [...filters];
    // DataSource will automatically re-filter when charts request data with new filters
    m.redraw();
  }

  view(): m.Children {
    return m('section', [
      m('h1', 'Declarative Charts with DataGrid'),
      m(
        'p',
        'Filter data using the DataGrid or brush the charts. All components share the same filter state.',
      ),

      this.renderSettings(),
      this.renderDataGrid(),
      this.renderChart(
        'count(value)',
        m(Histogram, {
          data: this.histogramLoader.use({
            bucketCount: 30,
            filters: this.filters,
          }).data,
          column: 'value',
          height: 300,
          xAxisLabel: 'value',
          yAxisLabel: 'count',
          filters: this.filters,
          onFiltersChanged: (filters) => this.updateFilters(filters),
        }),
      ),

      this.renderChart(
        'count(category)',
        m(BarChart, {
          data: this.barLoader.use({filters: this.filters}).data,
          height: 300,
          column: 'category',
          xAxisLabel: 'category',
          yAxisLabel: 'count',
          filters: this.filters,
          onFiltersChanged: (filters) => this.updateFilters(filters),
        }),
      ),

      this.renderChart(
        'cdf(value)',
        m(CDFChart, {
          data: this.cdfLoader.use({
            points: 100,
            filters: this.filters,
          }).data,
          column: 'value',
          height: 300,
          xAxisLabel: 'value',
          showPercentiles: this.showPercentiles,
          filters: this.filters,
          onFiltersChanged: (filters) => this.updateFilters(filters),
        }),
      ),

      this.renderChart(
        'x × y',
        m(ScatterPlot, {
          data: this.scatterLoader.use({
            filters: this.filters,
            computeCorrelation: this.showCorrelation,
          }).data,
          xColumn: 'x',
          yColumn: 'y',
          height: 300,
          xAxisLabel: 'x',
          yAxisLabel: 'y',
          showCorrelation: this.showCorrelation,
          filters: this.filters,
          onFiltersChanged: (filters) => this.updateFilters(filters),
        }),
      ),

      this.renderChart(
        'count(category) by group',
        m(GroupedBarChart, {
          data: this.groupedBarLoader.use({filters: this.filters}).data as
            | GroupedBarData
            | undefined,
          height: 300,
          column: 'category',
          xAxisLabel: 'category',
          yAxisLabel: 'count',
          filters: this.filters,
          onFiltersChanged: (filters) => this.updateFilters(filters),
        }),
      ),

      this.renderChart(
        'count(category) by group',
        m(StackedBarChart, {
          data: this.stackedBarLoader.use({filters: this.filters}).data as
            | GroupedBarData
            | undefined,
          height: 300,
          column: 'category',
          xAxisLabel: 'category',
          yAxisLabel: 'count',
          filters: this.filters,
          onFiltersChanged: (filters) => this.updateFilters(filters),
        }),
      ),

      this.renderChart(
        'cdf(value) by group',
        m(CDFChart, {
          data: this.groupedCDFLoader.use({
            points: 100,
            filters: this.filters,
          }).data,
          column: 'value',
          height: 300,
          xAxisLabel: 'value',
          showPercentiles: this.showPercentiles,
          filters: this.filters,
          onFiltersChanged: (filters) => this.updateFilters(filters),
        }),
      ),

      this.renderChart(
        'x × y by group',
        m(ScatterPlot, {
          data: this.coloredScatterLoader.use({
            filters: this.filters,
            computeCorrelation: this.showCorrelation,
          }).data,
          xColumn: 'x',
          yColumn: 'y',
          height: 300,
          xAxisLabel: 'x',
          yAxisLabel: 'y',
          showCorrelation: this.showCorrelation,
          filters: this.filters,
          onFiltersChanged: (filters) => this.updateFilters(filters),
        }),
      ),
    ]);
  }

  private renderSettings(): m.Children {
    return m(
      '.settings',
      {
        style: {
          padding: '16px',
          background: 'var(--pf-color-background-secondary)',
          borderRadius: '4px',
          marginBottom: '24px',
        },
      },
      [
        m('label', {style: {display: 'block', marginBottom: '8px'}}, [
          m('input[type=checkbox]', {
            checked: this.showCorrelation,
            onchange: (e: Event) => {
              this.showCorrelation = (e.target as HTMLInputElement).checked;
            },
          }),
          ' Show correlation line',
        ]),
        m('label', {style: {display: 'block', marginBottom: '8px'}}, [
          m('input[type=checkbox]', {
            checked: this.showPercentiles,
            onchange: (e: Event) => {
              this.showPercentiles = (e.target as HTMLInputElement).checked;
            },
          }),
          ' Show percentile markers',
        ]),
        m(
          'button',
          {
            style: {marginTop: '8px'},
            onclick: () => this.updateFilters([]),
          },
          'Clear All Filters',
        ),
      ],
    );
  }

  private renderDataGrid(): m.Children {
    return m(
      '.chart',
      {
        style: {
          background: 'var(--pf-color-background)',
          border: '1px solid var(--pf-color-border)',
          borderRadius: '4px',
          marginBottom: '24px',
          overflow: 'hidden',
        },
      },
      [
        m(
          '.datagrid-container',
          {
            style: {
              height: '300px',
            },
          },
          m(DataGrid, {
            data: this.dataSource,
            schema: SCHEMA,
            rootSchema: 'sample',
            fillHeight: true,
            filters: this.filters,
            initialColumns: [
              {id: 'id', field: 'id'},
              {id: 'category', field: 'category'},
              {id: 'value', field: 'value'},
              {id: 'group', field: 'group'},
            ],
            onFiltersChanged: (filters) => this.updateFilters(filters),
          }),
        ),
      ],
    );
  }

  private renderChart(title: string, chart: m.Children): m.Children {
    return m(
      '.chart',
      {
        style: {
          padding: '16px',
          background: 'var(--pf-color-background)',
          border: '1px solid var(--pf-color-border)',
          borderRadius: '4px',
          marginBottom: '48px',
        },
      },
      [
        m(
          'div',
          {
            style: {
              fontSize: '11px',
              color: 'var(--pf-color-text-secondary)',
              marginBottom: '4px',
              marginLeft: '24px',
              fontFamily: 'monospace',
            },
          },
          title,
        ),
        chart,
      ],
    );
  }
}
