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
import {Trace} from '../../public/trace';
import {Filter} from '../../components/widgets/datagrid/model';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import {createSimpleSchema} from '../../components/widgets/datagrid/sql_schema';
import {SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
import {
  Histogram,
  SQLHistogramLoader,
  SimpleBarChart as BarChart,
  SQLBarLoader,
  GroupedBarChart,
  StackedBarChart,
  GroupedBarData,
  CDFChart,
  SQLCDFLoader,
  ScatterPlot,
  SQLScatterLoader,
} from '../../components/charts/d3-decl';

interface DeclarativeChartsPageAttrs {
  trace: Trace;
}

/**
 * Demonstration page showing declarative charting with lifted state.
 *
 * Key architectural principles demonstrated:
 * 1. Parent component owns ALL state (no FilterStore)
 * 2. Charts receive data via attrs
 * 3. Charts emit events via callbacks
 * 4. State updates trigger Mithril re-renders
 * 5. Loaders handle async data fetching with caching
 */
export class DeclarativeChartsPage
  implements m.ClassComponent<DeclarativeChartsPageAttrs>
{
  // Loaders (persist across renders)
  private histogramLoader?: SQLHistogramLoader;
  private barLoader?: SQLBarLoader;
  private groupedBarLoader?: SQLBarLoader;
  private stackedBarLoader?: SQLBarLoader;
  private cdfLoader?: SQLCDFLoader;
  private groupedCDFLoader?: SQLCDFLoader;
  private scatterLoader?: SQLScatterLoader;
  private coloredScatterLoader?: SQLScatterLoader;
  private dataSource?: SQLDataSource;

  // Lifted state (owned by parent, passed to children)
  // Store ALL filters in one place for cross-filtering
  // Each chart can add/remove filters for its own fields
  private allFilters: Filter[] = [];

  // UI settings
  private showCorrelation = true;
  private showPercentiles = false;

  oninit({attrs}: m.Vnode<DeclarativeChartsPageAttrs>) {
    const engine = attrs.trace.engine;

    // Initialize DataGrid data source
    this.dataSource = new SQLDataSource({
      engine,
      sqlSchema: createSimpleSchema('slice'),
      rootSchemaName: 'query',
    });

    // Initialize loaders
    this.histogramLoader = new SQLHistogramLoader({
      engine,
      query: 'SELECT dur FROM slice WHERE dur > 0',
      valueColumn: 'dur',
    });

    this.barLoader = new SQLBarLoader({
      engine,
      query:
        'SELECT name, COUNT(*) as count FROM slice GROUP BY name ORDER BY count DESC LIMIT 20',
      categoryCol: 'name',
      valueCol: 'count',
    });

    // Grouped bar chart - count by name and depth (depth as color groups)
    this.groupedBarLoader = new SQLBarLoader({
      engine,
      query:
        'SELECT name, CAST(depth AS TEXT) as depth, COUNT(*) as count FROM slice WHERE depth BETWEEN 0 AND 3 GROUP BY name, depth ORDER BY count DESC LIMIT 20',
      categoryCol: 'name',
      valueCol: 'count',
      groupCol: 'depth',
    });

    // Stacked bar chart - same as grouped but will render stacked
    this.stackedBarLoader = new SQLBarLoader({
      engine,
      query:
        'SELECT name, CAST(depth AS TEXT) as depth, COUNT(*) as count FROM slice WHERE depth BETWEEN 0 AND 3 GROUP BY name, depth ORDER BY count DESC LIMIT 20',
      categoryCol: 'name',
      valueCol: 'count',
      groupCol: 'depth',
    });

    this.cdfLoader = new SQLCDFLoader({
      engine,
      query: 'SELECT dur FROM slice WHERE dur > 0',
      valueCol: 'dur',
    });

    // Grouped CDF - compare distributions by category
    this.groupedCDFLoader = new SQLCDFLoader({
      engine,
      query:
        "SELECT dur, CASE WHEN depth < 5 THEN 'Shallow' ELSE 'Deep' END as category FROM slice WHERE dur > 0",
      valueCol: 'dur',
      groupCol: 'category',
    });

    this.scatterLoader = new SQLScatterLoader({
      engine,
      query: 'SELECT dur, depth FROM slice WHERE dur > 0 LIMIT 1000',
      xCol: 'dur',
      yCol: 'depth',
    });

    this.coloredScatterLoader = new SQLScatterLoader({
      engine,
      query:
        "SELECT dur, depth, CASE WHEN depth < 5 THEN 'Shallow' ELSE 'Deep' END as category FROM slice WHERE dur > 0 LIMIT 1000",
      xCol: 'dur',
      yCol: 'depth',
      categoryCol: 'category',
    });
  }

  onremove() {
    // Clean up loaders
    this.histogramLoader?.dispose();
    this.barLoader?.dispose();
    this.groupedBarLoader?.dispose();
    this.stackedBarLoader?.dispose();
    this.cdfLoader?.dispose();
    this.groupedCDFLoader?.dispose();
    this.scatterLoader?.dispose();
    this.coloredScatterLoader?.dispose();
  }

  view() {
    return m('.declarative-charts-page', [
      m('h1', 'Declarative Charts with SQL Data'),
      m(
        'p',
        'Filter data using the DataGrid or brush the charts. All components share the same filter state.',
      ),

      this.renderSettings(),
      this.renderDataGrid(),

      this.renderChart('count(dur)', this.renderHistogram()),
      this.renderChart('count(name)', this.renderBarChart()),
      this.renderChart('cdf(dur)', this.renderCDF()),
      this.renderChart('dur × depth', this.renderScatter()),
      this.renderChart('count(name) by depth', this.renderGroupedBarChart()),
      this.renderChart(
        'count(name) by depth (stacked)',
        this.renderStackedBarChart(),
      ),
      this.renderChart('cdf(dur) by category', this.renderGroupedCDF()),
      this.renderChart('dur × depth by category', this.renderColoredScatter()),

      // Architecture notes
      m(
        '.architecture-notes',
        {
          style: {
            marginTop: '32px',
            padding: '16px',
            background: 'var(--pf-color-background-secondary)',
            borderRadius: '4px',
          },
        },
        [
          m('h3', 'Architecture Highlights'),
          m('ul', [
            m(
              'li',
              'State is lifted: All filters owned by this parent component',
            ),
            m(
              'li',
              'Charts are pure: Receive data via attrs, emit events via callbacks',
            ),
            m('li', 'No FilterStore: No global state coupling'),
            m(
              'li',
              'Mithril owns DOM: No d3.select(), all SVG via hyperscript',
            ),
            m('li', 'D3 as math: Only scales and path generators used'),
            m(
              'li',
              'Loaders handle async: QuerySlot provides caching and deduplication',
            ),
            m('li', 'Type safe: Zero any types, all interfaces well-defined'),
            m(
              'li',
              'SQL integration: Charts work seamlessly with SQL queries via loaders',
            ),
            m(
              'li',
              'Cross-filtering: All charts and DataGrid share the same filter state',
            ),
          ]),
        ],
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
            onclick: () => {
              this.allFilters = [];
            },
          },
          'Clear All Filters',
        ),
      ],
    );
  }

  private renderDataGrid(): m.Children {
    if (!this.dataSource) return null;

    const schema: SchemaRegistry = {
      slice: {
        id: {title: 'ID', columnType: 'quantitative'},
        name: {title: 'Name', columnType: 'text'},
        dur: {title: 'Duration', columnType: 'quantitative'},
        depth: {title: 'Depth', columnType: 'quantitative'},
        ts: {title: 'Timestamp', columnType: 'quantitative'},
      },
    };

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
            schema,
            rootSchema: 'slice',
            fillHeight: true,
            filters: this.allFilters,
            initialColumns: [
              {id: 'id', field: 'id'},
              {id: 'name', field: 'name'},
              {id: 'dur', field: 'dur'},
              {id: 'depth', field: 'depth'},
            ],
            onFiltersChanged: (filters) => {
              this.allFilters = [...filters];
            },
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

  private renderHistogram(): m.Children {
    if (!this.histogramLoader) return null;

    const {data} = this.histogramLoader.use({
      bucketCount: 30,
      filters: this.allFilters,
    });

    return m(Histogram, {
      data,
      column: 'dur',
      height: 300,
      xAxisLabel: 'dur',
      yAxisLabel: 'count',
      filters: this.allFilters,
      onFiltersChanged: (filters) => {
        this.allFilters = [...filters];
      },
    });
  }

  private renderBarChart(): m.Children {
    if (!this.barLoader) return null;

    const {data} = this.barLoader.use({
      filters: this.allFilters,
    });

    return m(BarChart, {
      data,
      height: 300,
      xAxisLabel: 'name',
      yAxisLabel: 'count',
      filters: this.allFilters,
      column: 'name',
      onFiltersChanged: (filters) => {
        this.allFilters = [...filters];
      },
    });
  }

  private renderCDF(): m.Children {
    if (!this.cdfLoader) return null;

    const {data} = this.cdfLoader.use({
      points: 100,
      filters: this.allFilters,
    });

    return m(CDFChart, {
      data,
      column: 'dur',
      height: 300,
      xAxisLabel: 'dur',
      showPercentiles: this.showPercentiles,
      filters: this.allFilters,
      onFiltersChanged: (filters) => {
        this.allFilters = [...filters];
      },
    });
  }

  private renderScatter(): m.Children {
    if (!this.scatterLoader) return null;

    const {data} = this.scatterLoader.use({
      filters: this.allFilters,
      computeCorrelation: this.showCorrelation,
    });

    return m(ScatterPlot, {
      data,
      xColumn: 'dur',
      yColumn: 'depth',
      height: 300,
      xAxisLabel: 'dur',
      yAxisLabel: 'depth',
      showCorrelation: this.showCorrelation,
      filters: this.allFilters,
      onFiltersChanged: (filters) => {
        this.allFilters = [...filters];
      },
    });
  }

  private renderGroupedBarChart(): m.Children {
    if (!this.groupedBarLoader) return null;

    const {data} = this.groupedBarLoader.use({
      filters: this.allFilters,
    });

    return m(GroupedBarChart, {
      data: data as GroupedBarData | undefined,
      height: 300,
      xAxisLabel: 'name',
      yAxisLabel: 'count',
      filters: this.allFilters,
      column: 'name',
      onFiltersChanged: (filters) => {
        this.allFilters = [...filters];
      },
    });
  }

  private renderStackedBarChart(): m.Children {
    if (!this.stackedBarLoader) return null;

    const {data} = this.stackedBarLoader.use({
      filters: this.allFilters,
    });

    return m(StackedBarChart, {
      data: data as GroupedBarData | undefined,
      height: 300,
      xAxisLabel: 'name',
      yAxisLabel: 'count',
      filters: this.allFilters,
      column: 'name',
      onFiltersChanged: (filters) => {
        this.allFilters = [...filters];
      },
    });
  }

  private renderGroupedCDF(): m.Children {
    if (!this.groupedCDFLoader) return null;

    const {data} = this.groupedCDFLoader.use({
      points: 100,
      filters: this.allFilters,
    });

    return m(CDFChart, {
      data,
      column: 'dur',
      height: 300,
      xAxisLabel: 'dur',
      showPercentiles: this.showPercentiles,
      filters: this.allFilters,
      onFiltersChanged: (filters) => {
        this.allFilters = [...filters];
      },
    });
  }

  private renderColoredScatter(): m.Children {
    if (!this.coloredScatterLoader) return null;

    const {data} = this.coloredScatterLoader.use({
      filters: this.allFilters,
      computeCorrelation: this.showCorrelation,
    });

    return m(ScatterPlot, {
      data,
      xColumn: 'dur',
      yColumn: 'depth',
      height: 300,
      xAxisLabel: 'dur',
      yAxisLabel: 'depth',
      showCorrelation: this.showCorrelation,
      filters: this.allFilters,
      onFiltersChanged: (filters) => {
        this.allFilters = [...filters];
      },
    });
  }
}
