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
import {
  Histogram,
  SQLHistogramLoader,
  SimpleBarChart,
  GroupedBarChart,
  StackedBarChart,
  SQLBarLoader,
  CDFChart,
  SQLCDFLoader,
  ScatterPlot,
  SQLScatterLoader,
  GroupedBarData,
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

  // Lifted state (owned by parent, passed to children)
  // Store ALL filters in one place for cross-filtering
  // Each chart can add/remove filters for its own fields
  private allFilters: Filter[] = [];

  // Settings
  private updateSourceChart = true;
  private showCorrelation = true;
  private showPercentiles = false;

  oninit({attrs}: m.Vnode<DeclarativeChartsPageAttrs>) {
    const engine = attrs.trace.engine;

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
      m('h1', 'Declarative Charting Demo'),
      m('p', [
        'This page demonstrates the new declarative charting architecture. ',
        'All state is lifted to the parent component, and charts are pure ',
        'functions that receive data via attrs and emit events via callbacks.',
      ]),

      // Settings panel
      m(
        '.settings-panel',
        {
          style: {
            padding: '16px',
            background: 'var(--pf-color-background-secondary)',
            marginBottom: '16px',
            borderRadius: '4px',
          },
        },
        [
          m('h3', 'Settings'),
          m('label', {style: {display: 'block', marginBottom: '8px'}}, [
            m('input[type=checkbox]', {
              checked: this.updateSourceChart,
              onchange: (e: Event) => {
                this.updateSourceChart = (e.target as HTMLInputElement).checked;
              },
            }),
            ' Update source chart on brush',
          ]),
          m('label', {style: {display: 'block', marginBottom: '8px'}}, [
            m('input[type=checkbox]', {
              checked: this.showCorrelation,
              onchange: (e: Event) => {
                this.showCorrelation = (e.target as HTMLInputElement).checked;
              },
            }),
            ' Show correlation line (scatter plot)',
          ]),
          m('label', {style: {display: 'block'}}, [
            m('input[type=checkbox]', {
              checked: this.showPercentiles,
              onchange: (e: Event) => {
                this.showPercentiles = (e.target as HTMLInputElement).checked;
              },
            }),
            ' Show percentile markers (CDF)',
          ]),
          m(
            'button',
            {
              style: {marginTop: '12px'},
              onclick: () => {
                this.allFilters = [];
              },
            },
            'Clear All Filters',
          ),
        ],
      ),

      // Charts grid
      m(
        '.charts-grid',
        {
          style: {
            display: 'grid',
            gridTemplateColumns: 'repeat(auto-fit, minmax(400px, 1fr))',
            gap: '24px',
          },
        },
        [
          // Histogram
          m(
            '.chart-container',
            {
              style: {
                padding: '16px',
                background: 'var(--pf-color-background)',
                border: '1px solid var(--pf-color-border)',
                borderRadius: '4px',
              },
            },
            [
              m('h3', 'Histogram - Slice Duration'),
              m(
                'p',
                {
                  style: {
                    fontSize: '12px',
                    color: 'var(--pf-color-text-muted)',
                  },
                },
                'Drag to select a range. State is lifted to parent.',
              ),
              this.renderHistogram(),
            ],
          ),

          // Bar Chart
          m(
            '.chart-container',
            {
              style: {
                padding: '16px',
                background: 'var(--pf-color-background)',
                border: '1px solid var(--pf-color-border)',
                borderRadius: '4px',
              },
            },
            [
              m('h3', 'Bar Chart - Top Slice Names'),
              m(
                'p',
                {
                  style: {
                    fontSize: '12px',
                    color: 'var(--pf-color-text-muted)',
                  },
                },
                'Click a bar to filter. No internal state.',
              ),
              this.renderBarChart(),
            ],
          ),

          // CDF Chart
          m(
            '.chart-container',
            {
              style: {
                padding: '16px',
                background: 'var(--pf-color-background)',
                border: '1px solid var(--pf-color-border)',
                borderRadius: '4px',
              },
            },
            [
              m('h3', 'CDF - Slice Duration Distribution'),
              m(
                'p',
                {
                  style: {
                    fontSize: '12px',
                    color: 'var(--pf-color-text-muted)',
                  },
                },
                'Cumulative distribution with crosshair tooltip. Brush to filter.',
              ),
              this.renderCDF(),
            ],
          ),

          // Scatter Plot
          m(
            '.chart-container',
            {
              style: {
                padding: '16px',
                background: 'var(--pf-color-background)',
                border: '1px solid var(--pf-color-border)',
                borderRadius: '4px',
              },
            },
            [
              m('h3', 'Scatter Plot - Duration vs Depth'),
              m(
                'p',
                {
                  style: {
                    fontSize: '12px',
                    color: 'var(--pf-color-text-muted)',
                  },
                },
                '2D brushing with correlation line. First 1000 slices.',
              ),
              this.renderScatter(),
            ],
          ),

          // Grouped Bar Chart
          m(
            '.chart-container',
            {
              style: {
                padding: '16px',
                background: 'var(--pf-color-background)',
                border: '1px solid var(--pf-color-border)',
                borderRadius: '4px',
              },
            },
            [
              m('h3', 'Grouped Bar Chart - Slices by Name & Track'),
              m(
                'p',
                {
                  style: {
                    fontSize: '12px',
                    color: 'var(--pf-color-text-muted)',
                  },
                },
                'Multiple series grouped side-by-side with legend.',
              ),
              this.renderGroupedBarChart(),
            ],
          ),

          // Stacked Bar Chart
          m(
            '.chart-container',
            {
              style: {
                padding: '16px',
                background: 'var(--pf-color-background)',
                border: '1px solid var(--pf-color-border)',
                borderRadius: '4px',
              },
            },
            [
              m('h3', 'Stacked Bar Chart - Slices by Name & Track'),
              m(
                'p',
                {
                  style: {
                    fontSize: '12px',
                    color: 'var(--pf-color-text-muted)',
                  },
                },
                'Multiple series stacked vertically with legend.',
              ),
              this.renderStackedBarChart(),
            ],
          ),

          // Grouped CDF
          m(
            '.chart-container',
            {
              style: {
                padding: '16px',
                background: 'var(--pf-color-background)',
                border: '1px solid var(--pf-color-border)',
                borderRadius: '4px',
              },
            },
            [
              m('h3', 'Grouped CDF - Duration by Depth Category'),
              m(
                'p',
                {
                  style: {
                    fontSize: '12px',
                    color: 'var(--pf-color-text-muted)',
                  },
                },
                'Compare distributions across categories with multiple CDF lines.',
              ),
              this.renderGroupedCDF(),
            ],
          ),

          // Colored Scatter Plot
          m(
            '.chart-container',
            {
              style: {
                padding: '16px',
                background: 'var(--pf-color-background)',
                border: '1px solid var(--pf-color-border)',
                borderRadius: '4px',
              },
            },
            [
              m('h3', 'Colored Scatter Plot - Duration vs Depth by Category'),
              m(
                'p',
                {
                  style: {
                    fontSize: '12px',
                    color: 'var(--pf-color-text-muted)',
                  },
                },
                'Points colored by category with legend. First 1000 slices.',
              ),
              this.renderColoredScatter(),
            ],
          ),
        ],
      ),

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
          ]),

          m('h4', {style: {marginTop: '16px'}}, 'DataGrid Integration'),
          m('p', {style: {marginTop: '8px'}}, [
            'To integrate charts with DataGrid, simply share the same ',
            m('code', 'filters: Filter[]'),
            ' state:',
          ]),
          m(
            'pre',
            {
              style: {
                background: 'var(--pf-color-background)',
                padding: '12px',
                borderRadius: '4px',
                overflow: 'auto',
                fontSize: '12px',
                marginTop: '8px',
              },
            },
            `class MyDashboard {
  private filters: Filter[] = [];  // Single source of truth
  
  view() {
    return [
      m(DataGrid, {
        data: dataSource,
        filters: this.filters,
        onFiltersChanged: (filters) => { this.filters = [...filters]; }
      }),
      
      m(Histogram, {
        data: histogramLoader.use({filters: this.filters}),
        filters: this.filters,
        onFiltersChanged: (filters) => { this.filters = [...filters]; }
      })
    ];
  }
}`,
          ),
          m(
            'p',
            {style: {marginTop: '8px', fontStyle: 'italic'}},
            'See the "Declarative Charts" demo in Widgets page for a working example with in-memory data.',
          ),
        ],
      ),
    ]);
  }

  private renderHistogram(): m.Children {
    if (!this.histogramLoader) return null;

    // Pass ALL filters directly to the loader for cross-filtering
    const {data} = this.histogramLoader.use({
      bucketCount: 30,
      filters: this.updateSourceChart ? this.allFilters : undefined,
    });

    return m(Histogram, {
      data,
      column: 'dur',
      height: 300,
      xAxisLabel: 'Duration (ns)',
      yAxisLabel: 'Count',
      filters: this.allFilters,
      onFiltersChanged: (filters) => {
        this.allFilters = [...filters];
        // No m.redraw() needed - Mithril auto-redraws
      },
    });
  }

  private renderBarChart(): m.Children {
    if (!this.barLoader) return null;

    // Pass ALL filters directly to the loader for cross-filtering
    const {data} = this.barLoader.use({
      filters: this.allFilters,
    });

    return m(SimpleBarChart, {
      data,
      height: 300,
      xAxisLabel: 'Slice Name',
      yAxisLabel: 'Count',
      filters: this.allFilters,
      column: 'name',
      onFiltersChanged: (filters: readonly Filter[]) => {
        this.allFilters = [...filters];
      },
    });
  }

  private renderCDF(): m.Children {
    if (!this.cdfLoader) return null;

    // Pass ALL filters directly to the loader for cross-filtering
    const {data} = this.cdfLoader.use({
      points: 100,
      filters: this.updateSourceChart ? this.allFilters : undefined,
    });

    return m(CDFChart, {
      data,
      column: 'dur',
      height: 300,
      xAxisLabel: 'Duration (ns)',
      showPercentiles: this.showPercentiles,
      filters: this.allFilters,
      onFiltersChanged: (filters) => {
        this.allFilters = [...filters];
      },
    });
  }

  private renderScatter(): m.Children {
    if (!this.scatterLoader) return null;

    // Pass ALL filters directly to the loader for cross-filtering
    const {data} = this.scatterLoader.use({
      filters: this.updateSourceChart ? this.allFilters : undefined,
      computeCorrelation: this.showCorrelation,
    });

    return m(ScatterPlot, {
      data,
      xColumn: 'dur',
      yColumn: 'depth',
      height: 300,
      xAxisLabel: 'Duration (ns)',
      yAxisLabel: 'Depth',
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
      xAxisLabel: 'Slice Name',
      yAxisLabel: 'Count',
      filters: this.allFilters,
      column: 'name',
      onFiltersChanged: (filters: readonly Filter[]) => {
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
      xAxisLabel: 'Slice Name',
      yAxisLabel: 'Count',
      filters: this.allFilters,
      column: 'name',
      onFiltersChanged: (filters: readonly Filter[]) => {
        this.allFilters = [...filters];
      },
    });
  }

  private renderGroupedCDF(): m.Children {
    if (!this.groupedCDFLoader) return null;

    const {data} = this.groupedCDFLoader.use({
      points: 100,
      filters: this.updateSourceChart ? this.allFilters : undefined,
    });

    return m(CDFChart, {
      data,
      column: 'dur',
      height: 300,
      xAxisLabel: 'Duration (ns)',
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
      filters: this.updateSourceChart ? this.allFilters : undefined,
      computeCorrelation: this.showCorrelation,
    });

    return m(ScatterPlot, {
      data,
      xColumn: 'dur',
      yColumn: 'depth',
      height: 300,
      xAxisLabel: 'Duration (ns)',
      yAxisLabel: 'Depth',
      showCorrelation: this.showCorrelation,
      filters: this.allFilters,
      onFiltersChanged: (filters) => {
        this.allFilters = [...filters];
      },
    });
  }
}
