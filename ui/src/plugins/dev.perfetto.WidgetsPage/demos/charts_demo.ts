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
import {
  BarChart,
  BarChartData,
  aggregateBarChartData,
} from '../../../components/widgets/charts/bar_chart';
import {AggregateFunction} from '../../../components/widgets/datagrid/model';
import {isIntegerAggregation} from '../../../components/widgets/charts/chart_utils';
import {
  SQLBarChartLoader,
  BarChartLoaderConfig,
} from '../../../components/widgets/charts/bar_chart_loader';
import {Histogram} from '../../../components/widgets/charts/histogram';
import {
  InMemoryHistogramLoader,
  SQLHistogramLoader,
  HistogramLoaderConfig,
} from '../../../components/widgets/charts/histogram_loader';
import {
  LineChart,
  LineChartData,
} from '../../../components/widgets/charts/line_chart';
import {
  SQLLineChartLoader,
  LineChartLoaderConfig,
} from '../../../components/widgets/charts/line_chart_loader';
import {
  PieChart,
  PieChartData,
} from '../../../components/widgets/charts/pie_chart';
import {
  SQLPieChartLoader,
  PieChartLoaderConfig,
} from '../../../components/widgets/charts/pie_chart_loader';
import {
  Scatterplot,
  ScatterChartData,
} from '../../../components/widgets/charts/scatterplot';
import {
  SQLScatterChartLoader,
  ScatterChartLoaderConfig,
} from '../../../components/widgets/charts/scatterplot_loader';
import {Treemap, TreemapData} from '../../../components/widgets/charts/treemap';
import {
  SQLTreemapLoader,
  TreemapLoaderConfig,
} from '../../../components/widgets/charts/treemap_loader';
import {
  SQLCdfLoader,
  CdfLoaderConfig,
} from '../../../components/widgets/charts/cdf_loader';
import {
  BoxplotChart,
  BoxplotData,
} from '../../../components/widgets/charts/boxplot';
import {
  SQLBoxplotLoader,
  BoxplotLoaderConfig,
} from '../../../components/widgets/charts/boxplot_loader';
import {
  HeatmapChart,
  HeatmapData,
} from '../../../components/widgets/charts/heatmap';
import {
  SQLHeatmapLoader,
  HeatmapLoaderConfig,
} from '../../../components/widgets/charts/heatmap_loader';
import {App} from '../../../public/app';
import {EnumOption, renderWidgetShowcase} from '../widgets_page_utils';
import {Trace} from '../../../public/trace';

// Generate sample data with normal distribution
function generateNormalData(
  count: number,
  mean: number,
  stdDev: number,
  integer = false,
): number[] {
  const data: number[] = [];
  for (let i = 0; i < count; i++) {
    // Box-Muller transform for normal distribution
    const u1 = Math.random();
    const u2 = Math.random();
    const z = Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
    const v = mean + z * stdDev;
    data.push(integer ? Math.round(v) : v);
  }
  return data;
}

interface SampleSlice {
  readonly process: string;
  readonly dur: number;
}

// Simulated raw trace data: slices with process name and duration.
const SAMPLE_SLICES: readonly SampleSlice[] = [
  {process: 'Chrome', dur: 1500},
  {process: 'Chrome', dur: 2300},
  {process: 'Chrome', dur: 800},
  {process: 'Chrome', dur: 1500},
  {process: 'Chrome', dur: 4200},
  {process: 'SurfaceFlinger', dur: 500},
  {process: 'SurfaceFlinger', dur: 1200},
  {process: 'SurfaceFlinger', dur: 900},
  {process: 'SurfaceFlinger', dur: 500},
  {process: 'SystemUI', dur: 3100},
  {process: 'SystemUI', dur: 700},
  {process: 'SystemUI', dur: 2200},
  {process: 'Launcher', dur: 1800},
  {process: 'Launcher', dur: 600},
  {process: 'InputDispatcher', dur: 200},
  {process: 'InputDispatcher', dur: 350},
  {process: 'InputDispatcher', dur: 200},
  {process: 'AudioFlinger', dur: 900},
  {process: 'AudioFlinger', dur: 1100},
  {process: 'CameraService', dur: 5000},
];

export function renderCharts(app: App): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Charts'),
      m('p', [
        'ECharts-based chart components for visualizing data. ',
        'Includes Bar, Line, Pie/Donut, Histogram, Scatter, Treemap, CDF, Boxplot, and Heatmap charts.',
      ]),
    ),

    // LineChart section
    m('h2', 'LineChart'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(LineChartDemo, {
          height: opts.height,
          enableBrush: opts.enableBrush,
          logScale: opts.logScale,
          showPoints: opts.showPoints,
          multiSeries: opts.multiSeries,
        });
      },
      initialOpts: {
        height: 250,
        enableBrush: true,
        logScale: false,
        showPoints: true,
        multiSeries: false,
      },
    }),

    // PieChart section
    m('h2', {style: {marginTop: '32px'}}, 'PieChart'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(PieChartDemo, {
          height: opts.height,
          showLabels: opts.showLabels,
          showLegend: opts.showLegend,
          donut: opts.donut,
        });
      },
      initialOpts: {
        height: 250,
        showLabels: false,
        showLegend: true,
        donut: false,
      },
    }),

    // BarChart section
    m('h2', {style: {marginTop: '32px'}}, 'BarChart'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(BarChartDemo, {
          height: opts.height,
          logScale: opts.logScale,
          enableBrush: opts.enableBrush,
          horizontal: opts.horizontal,
          aggregation: opts.aggregation as AggregateFunction,
        });
      },
      initialOpts: {
        height: 250,
        logScale: false,
        enableBrush: true,
        horizontal: false,
        aggregation: new EnumOption('SUM', [
          'SUM',
          'AVG',
          'MIN',
          'MAX',
          'COUNT_DISTINCT',
        ] as const),
      },
    }),

    // Histogram section
    m('h2', {style: {marginTop: '32px'}}, 'Histogram'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(HistogramDemo, {
          bucketCount: opts.bucketCount,
          height: opts.height,
          enableBrush: opts.enableBrush,
          logScale: opts.logScale,
          integer: opts.integer,
        });
      },
      initialOpts: {
        bucketCount: 20,
        height: 250,
        enableBrush: true,
        logScale: false,
        integer: false,
      },
    }),

    // ScatterChart section
    m('h2', {style: {marginTop: '32px'}}, 'ScatterChart'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(ScatterChartDemo, {
          height: opts.height,
          showLegend: opts.showLegend,
          bubbleMode: opts.bubbleMode,
          scaleAxes: opts.scaleAxes,
        });
      },
      initialOpts: {
        height: 250,
        showLegend: true,
        bubbleMode: false,
        scaleAxes: false,
      },
    }),

    // TreemapChart section
    m('h2', {style: {marginTop: '32px'}}, 'TreemapChart'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(TreemapChartDemo, {
          height: opts.height,
          showLabels: opts.showLabels,
          hierarchical: opts.hierarchical,
        });
      },
      initialOpts: {
        height: 300,
        showLabels: true,
        hierarchical: true,
      },
    }),

    // BoxplotChart section
    m('h2', {style: {marginTop: '32px'}}, 'BoxplotChart'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(BoxplotChartDemo, {
          height: opts.height,
          horizontal: opts.horizontal,
        });
      },
      initialOpts: {
        height: 300,
        horizontal: false,
      },
    }),

    // HeatmapChart section
    m('h2', {style: {marginTop: '32px'}}, 'HeatmapChart'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(HeatmapChartDemo, {
          height: opts.height,
        });
      },
      initialOpts: {
        height: 300,
      },
    }),

    // SQL loader demos (only shown when a trace is loaded)
    ...renderSQLDemos(app),
    app.trace === undefined &&
      m(
        'p',
        {style: {marginTop: '32px', color: 'var(--pf-color-text-muted)'}},
        'Load a trace to see the SQL loader demos.',
      ),
  ];
}

function renderSQLDemos(app: App): m.Children[] {
  const trace = app.trace;
  if (trace === undefined) return [];
  return [
    m('h3', {style: {marginTop: '32px'}}, 'SQLBarChartLoader'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(SQLBarChartDemo, {
          trace,
          height: opts.height,
          enableBrush: opts.enableBrush,
          logScale: opts.logScale,
          horizontal: opts.horizontal,
          aggregation: opts.aggregation as AggregateFunction,
        });
      },
      initialOpts: {
        height: 250,
        enableBrush: true,
        logScale: false,
        horizontal: false,
        aggregation: new EnumOption('SUM', [
          'SUM',
          'AVG',
          'MIN',
          'MAX',
          'COUNT_DISTINCT',
        ] as const),
      },
    }),
    m('h3', {style: {marginTop: '32px'}}, 'SQLLineChartLoader'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(SQLLineChartDemo, {
          trace,
          height: opts.height,
          enableBrush: opts.enableBrush,
          showPoints: opts.showPoints,
          maxPoints: opts.maxPoints,
          scaleAxes: opts.scaleAxes,
        });
      },
      initialOpts: {
        height: 250,
        enableBrush: true,
        showPoints: true,
        maxPoints: 200,
        scaleAxes: true,
      },
    }),
    m('h3', {style: {marginTop: '32px'}}, 'SQLPieChartLoader'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(SQLPieChartDemo, {
          trace,
          height: opts.height,
          showLegend: opts.showLegend,
          donut: opts.donut,
          aggregation: opts.aggregation as AggregateFunction,
          limit: opts.limit,
        });
      },
      initialOpts: {
        height: 250,
        showLegend: true,
        donut: false,
        aggregation: new EnumOption('SUM', [
          'SUM',
          'AVG',
          'MIN',
          'MAX',
          'COUNT_DISTINCT',
        ] as const),
        limit: 8,
      },
    }),
    m('h3', {style: {marginTop: '32px'}}, 'SQLHistogramLoader'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(SQLHistogramDemo, {
          trace,
          bucketCount: opts.bucketCount,
          height: opts.height,
          enableBrush: opts.enableBrush,
          logScale: opts.logScale,
        });
      },
      initialOpts: {
        bucketCount: 20,
        height: 250,
        enableBrush: true,
        logScale: false,
      },
    }),
    m('h3', {style: {marginTop: '32px'}}, 'SQLScatterChartLoader'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(SQLScatterChartDemo, {
          trace,
          height: opts.height,
          showLegend: opts.showLegend,
          maxPoints: opts.maxPoints,
          scaleAxes: opts.scaleAxes,
        });
      },
      initialOpts: {
        height: 250,
        showLegend: true,
        maxPoints: 500,
        scaleAxes: true,
      },
    }),
    m('h3', {style: {marginTop: '32px'}}, 'SQLTreemapLoader'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(SQLTreemapDemo, {
          trace,
          height: opts.height,
          showLabels: opts.showLabels,
          limit: opts.limit,
        });
      },
      initialOpts: {
        height: 300,
        showLabels: true,
        limit: 10,
      },
    }),
    m('h3', {style: {marginTop: '32px'}}, 'SQLCdfLoader'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(SQLCdfDemo, {
          trace,
          height: opts.height,
          maxPoints: opts.maxPoints,
          enableBrush: opts.enableBrush,
        });
      },
      initialOpts: {
        height: 250,
        maxPoints: 500,
        enableBrush: true,
      },
    }),
    m('h3', {style: {marginTop: '32px'}}, 'SQLBoxplotLoader'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(SQLBoxplotDemo, {
          trace,
          height: opts.height,
          limit: opts.limit,
        });
      },
      initialOpts: {
        height: 300,
        limit: 10,
      },
    }),
    m('h3', {style: {marginTop: '32px'}}, 'SQLHeatmapLoader'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(SQLHeatmapDemo, {
          trace,
          height: opts.height,
          xLimit: opts.xLimit,
          yLimit: opts.yLimit,
        });
      },
      initialOpts: {
        height: 300,
        xLimit: 15,
        yLimit: 15,
      },
    }),
  ];
}

function HistogramDemo(): m.Component<{
  bucketCount: number;
  height: number;
  enableBrush: boolean;
  logScale: boolean;
  integer: boolean;
}> {
  const continuousData = generateNormalData(1000, 50, 15);
  const integerData = generateNormalData(1000, 50, 15, true);

  const continuousLoader = new InMemoryHistogramLoader(continuousData);
  const integerLoader = new InMemoryHistogramLoader(integerData);

  let showcaseFilter: {min: number; max: number} | undefined;

  return {
    view: ({attrs}) => {
      const loader = attrs.integer ? integerLoader : continuousLoader;
      const config: HistogramLoaderConfig = {
        bucketCount: attrs.bucketCount,
        integer: attrs.integer || undefined,
        filter: showcaseFilter,
      };
      const {data} = loader.use(config);
      return m('div', [
        m(Histogram, {
          data,
          height: attrs.height,
          xAxisLabel: attrs.integer ? 'Thread Count' : 'Value',
          yAxisLabel: 'Count',
          logScale: attrs.logScale,
          integerDimension: attrs.integer,
          onBrush: attrs.enableBrush
            ? (range) => {
                showcaseFilter = {min: range.start, max: range.end};
              }
            : undefined,
        }),
        m(
          'pre',
          {
            style: {
              marginTop: '8px',
              fontSize: '11px',
              background: 'var(--pf-color-background-secondary)',
              padding: '8px',
              borderRadius: '4px',
            },
          },
          `loader.use(${JSON.stringify(config, null, 2)})`,
        ),
        showcaseFilter &&
          m(
            'button',
            {
              style: {marginTop: '8px', fontSize: '12px'},
              onclick: () => {
                showcaseFilter = undefined;
              },
            },
            'Clear filter',
          ),
      ]);
    },
  };
}

function BarChartDemo(): m.Component<{
  height: number;
  logScale: boolean;
  enableBrush: boolean;
  horizontal: boolean;
  aggregation: AggregateFunction;
}> {
  let brushedLabels: Array<string | number> | undefined;

  return {
    view: ({attrs}) => {
      const {aggregation} = attrs;
      let data: BarChartData = aggregateBarChartData(
        SAMPLE_SLICES,
        (s) => s.process,
        (s) => s.dur,
        aggregation,
      );

      // Filter by brushed labels
      const labels = brushedLabels;
      if (labels !== undefined) {
        data = {
          items: data.items.filter((item) => labels.includes(item.label)),
        };
      }

      const measureLabels: Record<AggregateFunction, string> = {
        ANY: 'Any Duration',
        SUM: 'Total Duration',
        AVG: 'Avg Duration',
        MIN: 'Min Duration',
        MAX: 'Max Duration',
        COUNT_DISTINCT: 'Distinct Durations',
      };

      return m('div', [
        m(BarChart, {
          data,
          height: attrs.height,
          dimensionLabel: 'Process',
          measureLabel: measureLabels[aggregation],
          integerMeasure: isIntegerAggregation(aggregation),
          logScale: attrs.logScale,
          orientation: attrs.horizontal ? 'horizontal' : 'vertical',
          onBrush: attrs.enableBrush
            ? (labels) => {
                brushedLabels = labels;
              }
            : undefined,
        }),
        m(
          'pre',
          {
            style: {
              marginTop: '8px',
              fontSize: '11px',
              background: 'var(--pf-color-background-secondary)',
              padding: '8px',
              borderRadius: '4px',
            },
          },
          brushedLabels
            ? `Brushed: [${brushedLabels.join(', ')}]`
            : 'Drag to brush-select bars',
        ),
        brushedLabels &&
          m(
            'button',
            {
              style: {marginTop: '8px', fontSize: '12px'},
              onclick: () => {
                brushedLabels = undefined;
              },
            },
            'Clear filter',
          ),
      ]);
    },
  };
}

function SQLBarChartDemo(): m.Component<{
  trace: Trace;
  height: number;
  enableBrush: boolean;
  logScale: boolean;
  horizontal: boolean;
  aggregation: AggregateFunction;
}> {
  let loader: SQLBarChartLoader | undefined;
  let brushedLabels: Array<string | number> | undefined;

  return {
    view: ({attrs}) => {
      if (!loader) {
        loader = new SQLBarChartLoader({
          engine: attrs.trace.engine,
          query: 'SELECT name, dur FROM slice WHERE dur > 0',
          dimensionColumn: 'name',
          measureColumn: 'dur',
        });
      }

      const {aggregation} = attrs;
      const config: BarChartLoaderConfig = {
        aggregation,
        limit: 10,
        filter: brushedLabels,
      };
      const {data, isPending} = loader.use(config);

      const measureLabels: Record<AggregateFunction, string> = {
        ANY: 'Any Duration',
        SUM: 'Total Duration',
        AVG: 'Avg Duration',
        MIN: 'Min Duration',
        MAX: 'Max Duration',
        COUNT_DISTINCT: 'Distinct Durations',
      };

      return m('div', [
        m(BarChart, {
          data,
          height: attrs.height,
          dimensionLabel: 'Slice Name',
          measureLabel: measureLabels[aggregation],
          integerMeasure: isIntegerAggregation(aggregation),
          logScale: attrs.logScale,
          orientation: attrs.horizontal ? 'horizontal' : 'vertical',
          onBrush: attrs.enableBrush
            ? (labels) => {
                brushedLabels = labels;
              }
            : undefined,
        }),
        m(
          'pre',
          {
            style: {
              marginTop: '8px',
              fontSize: '11px',
              background: 'var(--pf-color-background-secondary)',
              padding: '8px',
              borderRadius: '4px',
            },
          },
          [
            `query: 'SELECT name, dur FROM slice WHERE dur > 0'\n`,
            `dimensionColumn: 'name', measureColumn: 'dur'\n`,
            `loader.use(${JSON.stringify(config, null, 2)})`,
            isPending ? '\n(loading...)' : '',
          ],
        ),
        brushedLabels &&
          m(
            'button',
            {
              style: {marginTop: '8px', fontSize: '12px'},
              onclick: () => {
                brushedLabels = undefined;
              },
            },
            'Clear filter',
          ),
      ]);
    },
    onremove: () => {
      loader?.dispose();
      loader = undefined;
    },
  };
}

function SQLHistogramDemo(): m.Component<{
  trace: Trace;
  bucketCount: number;
  height: number;
  enableBrush: boolean;
  logScale: boolean;
}> {
  let loader: SQLHistogramLoader | undefined;
  let filter: {min: number; max: number} | undefined;

  return {
    view: ({attrs}) => {
      // Create loader on first render (or if trace changes)
      if (!loader) {
        loader = new SQLHistogramLoader({
          engine: attrs.trace.engine,
          query: 'SELECT dur FROM slice WHERE dur > 0',
          valueColumn: 'dur',
        });
      }

      const config: HistogramLoaderConfig = {
        bucketCount: attrs.bucketCount,
        filter,
      };
      const {data, isPending} = loader.use(config);

      return m('div', [
        m(Histogram, {
          data,
          height: attrs.height,
          xAxisLabel: 'Duration (ns)',
          yAxisLabel: 'Count',
          logScale: attrs.logScale,
          onBrush: attrs.enableBrush
            ? (range) => {
                filter = {min: range.start, max: range.end};
              }
            : undefined,
        }),
        m(
          'pre',
          {
            style: {
              marginTop: '8px',
              fontSize: '11px',
              background: 'var(--pf-color-background-secondary)',
              padding: '8px',
              borderRadius: '4px',
            },
          },
          [
            `query: 'SELECT dur FROM slice WHERE dur > 0'\n`,
            `valueColumn: 'dur'\n`,
            `loader.use(${JSON.stringify(config, null, 2)})`,
            isPending ? '\n(loading...)' : '',
          ],
        ),
        filter &&
          m(
            'button',
            {
              style: {marginTop: '8px', fontSize: '12px'},
              onclick: () => {
                filter = undefined;
              },
            },
            'Clear filter',
          ),
      ]);
    },
    onremove: () => {
      loader?.dispose();
      loader = undefined;
    },
  };
}

function SQLLineChartDemo(): m.Component<{
  trace: Trace;
  height: number;
  enableBrush: boolean;
  showPoints: boolean;
  maxPoints: number;
  scaleAxes: boolean;
}> {
  let loader: SQLLineChartLoader | undefined;
  let xRange: {min: number; max: number} | undefined;

  return {
    view: ({attrs}) => {
      if (!loader) {
        loader = new SQLLineChartLoader({
          engine: attrs.trace.engine,
          query: 'SELECT ts, dur, name FROM slice WHERE dur > 0 LIMIT 500',
          xColumn: 'ts',
          yColumn: 'dur',
        });
      }

      const config: LineChartLoaderConfig = {
        xRange,
        maxPoints: attrs.maxPoints,
      };
      const {data, isPending} = loader.use(config);

      return m('div', [
        m(LineChart, {
          data,
          height: attrs.height,
          xAxisLabel: 'Timestamp',
          yAxisLabel: 'Value',
          showPoints: attrs.showPoints,
          scaleAxes: attrs.scaleAxes,
          onBrush: attrs.enableBrush
            ? (range) => {
                xRange = {min: range.start, max: range.end};
              }
            : undefined,
        }),
        m(
          'pre',
          {
            style: {
              marginTop: '8px',
              fontSize: '11px',
              background: 'var(--pf-color-background-secondary)',
              padding: '8px',
              borderRadius: '4px',
            },
          },
          [
            `query: 'SELECT ts, dur, name FROM slice WHERE dur > 0 LIMIT 500'\n`,
            `xColumn: 'ts', yColumn: 'dur'\n`,
            `loader.use(${JSON.stringify(config, null, 2)})`,
            isPending ? '\n(loading...)' : '',
          ],
        ),
        xRange &&
          m(
            'button',
            {
              style: {marginTop: '8px', fontSize: '12px'},
              onclick: () => {
                xRange = undefined;
              },
            },
            'Clear filter',
          ),
      ]);
    },
    onremove: () => {
      loader?.dispose();
      loader = undefined;
    },
  };
}

function SQLPieChartDemo(): m.Component<{
  trace: Trace;
  height: number;
  showLegend: boolean;
  donut: boolean;
  aggregation: AggregateFunction;
  limit: number;
}> {
  let loader: SQLPieChartLoader | undefined;

  return {
    view: ({attrs}) => {
      if (!loader) {
        loader = new SQLPieChartLoader({
          engine: attrs.trace.engine,
          query: 'SELECT name, dur FROM slice WHERE dur > 0',
          dimensionColumn: 'name',
          measureColumn: 'dur',
        });
      }

      const config: PieChartLoaderConfig = {
        aggregation: attrs.aggregation,
        limit: attrs.limit,
      };
      const {data, isPending} = loader.use(config);

      return m('div', [
        m(PieChart, {
          data,
          height: attrs.height,
          showLegend: attrs.showLegend,
          innerRadiusRatio: attrs.donut ? 0.5 : 0,
        }),
        m(
          'pre',
          {
            style: {
              marginTop: '8px',
              fontSize: '11px',
              background: 'var(--pf-color-background-secondary)',
              padding: '8px',
              borderRadius: '4px',
            },
          },
          [
            `query: 'SELECT name, dur FROM slice WHERE dur > 0'\n`,
            `dimensionColumn: 'name', measureColumn: 'dur'\n`,
            `loader.use(${JSON.stringify(config, null, 2)})`,
            isPending ? '\n(loading...)' : '',
          ],
        ),
      ]);
    },
    onremove: () => {
      loader?.dispose();
      loader = undefined;
    },
  };
}

// Static sample data for LineChart demo (generated once)
const LINE_CHART_SAMPLE_DATA = generateLineChartSampleData();
const LINE_CHART_MULTI_SERIES_DATA = generateMultiSeriesLineData();

function generateLineChartSampleData(): LineChartData {
  const points = [];
  for (let i = 0; i < 20; i++) {
    points.push({
      x: i,
      y: Math.sin(i * 0.5) * 50 + 50 + Math.random() * 10,
    });
  }
  return {
    series: [{name: 'Values', points}],
  };
}

function generateMultiSeriesLineData(): LineChartData {
  const series1 = [];
  const series2 = [];
  for (let i = 0; i < 20; i++) {
    series1.push({
      x: i,
      y: Math.sin(i * 0.5) * 30 + 50 + Math.random() * 5,
    });
    series2.push({
      x: i,
      y: Math.cos(i * 0.5) * 25 + 60 + Math.random() * 5,
    });
  }
  return {
    series: [
      {name: 'Series A', points: series1},
      {name: 'Series B', points: series2},
    ],
  };
}

function LineChartDemo(): m.Component<{
  height: number;
  enableBrush: boolean;
  logScale: boolean;
  showPoints: boolean;
  multiSeries: boolean;
}> {
  let brushRange: {start: number; end: number} | undefined;

  // Helper to interpolate Y value between two points at a given X
  function interpolateY(
    p1: {x: number; y: number},
    p2: {x: number; y: number},
    x: number,
  ): number {
    if (p1.x === p2.x) return p1.y;
    const t = (x - p1.x) / (p2.x - p1.x);
    return p1.y + t * (p2.y - p1.y);
  }

  // Filter points to range, with interpolation at boundaries for continuity
  function filterPointsWithInterpolation(
    points: ReadonlyArray<{x: number; y: number}>,
    start: number,
    end: number,
  ): Array<{x: number; y: number}> {
    if (points.length === 0) return [];

    // Sort points by X (should already be sorted, but just in case)
    const sorted = [...points].sort((a, b) => a.x - b.x);

    const result: Array<{x: number; y: number}> = [];

    // Find points within range and interpolate at boundaries
    for (let i = 0; i < sorted.length; i++) {
      const curr = sorted[i];
      const prev = i > 0 ? sorted[i - 1] : undefined;

      // Add interpolated start point if we're crossing into the range
      if (prev !== undefined && prev.x < start && curr.x >= start) {
        if (curr.x > start) {
          result.push({x: start, y: interpolateY(prev, curr, start)});
        }
      }

      // Add current point if within range
      if (curr.x >= start && curr.x <= end) {
        result.push({x: curr.x, y: curr.y});
      }

      // Add interpolated end point if we're leaving the range
      const next = i < sorted.length - 1 ? sorted[i + 1] : undefined;
      if (next !== undefined && curr.x <= end && next.x > end) {
        if (curr.x < end) {
          result.push({x: end, y: interpolateY(curr, next, end)});
        }
      }
    }

    return result;
  }

  return {
    view: ({attrs}) => {
      const fullData = attrs.multiSeries
        ? LINE_CHART_MULTI_SERIES_DATA
        : LINE_CHART_SAMPLE_DATA;

      // Filter data to the brushed X range with interpolation at boundaries
      const range = brushRange;
      const data: LineChartData =
        range !== undefined
          ? {
              series: fullData.series.map((s) => ({
                ...s,
                points: filterPointsWithInterpolation(
                  s.points,
                  range.start,
                  range.end,
                ),
              })),
            }
          : fullData;

      return m('div', [
        m(LineChart, {
          data,
          height: attrs.height,
          xAxisLabel: 'Time',
          yAxisLabel: 'Value',
          logScale: attrs.logScale,
          showPoints: attrs.showPoints,
          xAxisMin: range?.start,
          xAxisMax: range?.end,
          onBrush: attrs.enableBrush
            ? (newRange) => {
                brushRange = newRange;
              }
            : undefined,
        }),
        m(
          'pre',
          {
            style: {
              marginTop: '8px',
              fontSize: '11px',
              background: 'var(--pf-color-background-secondary)',
              padding: '8px',
              borderRadius: '4px',
            },
          },
          brushRange
            ? `Brushed range: [${brushRange.start.toFixed(2)}, ${brushRange.end.toFixed(2)}]`
            : 'Drag to brush-select a range',
        ),
        brushRange &&
          m(
            'button',
            {
              style: {marginTop: '8px', fontSize: '12px'},
              onclick: () => {
                brushRange = undefined;
              },
            },
            'Clear selection',
          ),
      ]);
    },
  };
}

// Static sample data for PieChart demo
const PIE_CHART_SAMPLE_DATA: PieChartData = {
  slices: [
    {label: 'Chrome', value: 35},
    {label: 'Firefox', value: 25},
    {label: 'Safari', value: 20},
    {label: 'Edge', value: 15},
    {label: 'Other', value: 5},
  ],
};

function PieChartDemo(): m.Component<{
  height: number;
  showLabels: boolean;
  showLegend: boolean;
  donut: boolean;
}> {
  let clickedSlice: string | undefined;

  return {
    view: ({attrs}) => {
      return m('div', [
        m(PieChart, {
          data: PIE_CHART_SAMPLE_DATA,
          height: attrs.height,
          showLabels: attrs.showLabels,
          showLegend: attrs.showLegend,
          innerRadiusRatio: attrs.donut ? 0.5 : 0,
          onSliceClick: (slice) => {
            clickedSlice = slice.label;
          },
        }),
        m(
          'pre',
          {
            style: {
              marginTop: '8px',
              fontSize: '11px',
              background: 'var(--pf-color-background-secondary)',
              padding: '8px',
              borderRadius: '4px',
            },
          },
          clickedSlice
            ? `Clicked: ${clickedSlice}`
            : 'Click a slice to select it',
        ),
        clickedSlice &&
          m(
            'button',
            {
              style: {marginTop: '8px', fontSize: '12px'},
              onclick: () => {
                clickedSlice = undefined;
              },
            },
            'Clear selection',
          ),
      ]);
    },
  };
}

// Simple seeded pseudo-random number generator for reproducible demo data.
function seededRandom(seed: number): () => number {
  let s = seed;
  return () => {
    s = (s * 16807) % 2147483647;
    return s / 2147483647;
  };
}

// Static sample data for ScatterChart demo
const SCATTER_SAMPLE_DATA: ScatterChartData = (() => {
  const rng = seededRandom(42);
  return {
    series: [
      {
        name: 'Group A',
        points: Array.from({length: 30}, () => ({
          x: rng() * 100,
          y: rng() * 50 + 25,
        })),
      },
      {
        name: 'Group B',
        points: Array.from({length: 25}, () => ({
          x: rng() * 100,
          y: rng() * 50 + 50,
        })),
      },
    ],
  };
})();

const SCATTER_BUBBLE_DATA: ScatterChartData = {
  series: [
    {
      name: 'Processes',
      points: [
        {x: 10, y: 20, size: 100, label: 'Chrome'},
        {x: 25, y: 45, size: 200, label: 'Firefox'},
        {x: 40, y: 30, size: 150, label: 'Safari'},
        {x: 55, y: 60, size: 80, label: 'Edge'},
        {x: 70, y: 35, size: 300, label: 'System'},
        {x: 85, y: 50, size: 120, label: 'Launcher'},
      ],
    },
  ],
};

function ScatterChartDemo(): m.Component<{
  height: number;
  showLegend: boolean;
  bubbleMode: boolean;
  scaleAxes: boolean;
}> {
  return {
    view: ({attrs}) => {
      const data = attrs.bubbleMode ? SCATTER_BUBBLE_DATA : SCATTER_SAMPLE_DATA;
      return m('div', [
        m(Scatterplot, {
          data,
          height: attrs.height,
          xAxisLabel: 'X Value',
          yAxisLabel: 'Y Value',
          showLegend: attrs.showLegend,
          scaleAxes: attrs.scaleAxes,
        }),
        m(
          'pre',
          {
            style: {
              marginTop: '8px',
              fontSize: '11px',
              background: 'var(--pf-color-background-secondary)',
              padding: '8px',
              borderRadius: '4px',
            },
          },
          [
            attrs.bubbleMode
              ? 'Bubble mode: size encodes a third dimension'
              : 'Regular scatter plot with two series',
            attrs.scaleAxes
              ? '\nscaleAxes: true (axis range from data min/max)'
              : '\nscaleAxes: false (axis range includes zero)',
          ],
        ),
      ]);
    },
  };
}

// Static sample data for TreemapChart demo
const TREEMAP_FLAT_DATA: TreemapData = {
  nodes: [
    {name: 'Chrome', value: 350},
    {name: 'SurfaceFlinger', value: 150},
    {name: 'SystemUI', value: 200},
    {name: 'Launcher', value: 100},
    {name: 'InputDispatcher', value: 75},
    {name: 'AudioFlinger', value: 125},
  ],
};

const TREEMAP_HIERARCHICAL_DATA: TreemapData = {
  nodes: [
    {
      name: 'UI Processes',
      value: 650, // Sum of children: 350 + 200 + 100
      category: 'ui',
      children: [
        {name: 'Chrome', value: 350, category: 'ui'},
        {name: 'SystemUI', value: 200, category: 'ui'},
        {name: 'Launcher', value: 100, category: 'ui'},
      ],
    },
    {
      name: 'System Services',
      value: 350, // Sum of children: 150 + 75 + 125
      category: 'system',
      children: [
        {name: 'SurfaceFlinger', value: 150, category: 'system'},
        {name: 'InputDispatcher', value: 75, category: 'system'},
        {name: 'AudioFlinger', value: 125, category: 'system'},
      ],
    },
  ],
};

function TreemapChartDemo(): m.Component<{
  height: number;
  showLabels: boolean;
  hierarchical: boolean;
}> {
  let clickedNode: string | undefined;

  return {
    view: ({attrs}) => {
      const data = attrs.hierarchical
        ? TREEMAP_HIERARCHICAL_DATA
        : TREEMAP_FLAT_DATA;
      return m('div', [
        m(Treemap, {
          data,
          height: attrs.height,
          showLabels: attrs.showLabels,
          onNodeClick: (node) => {
            clickedNode = node.name;
          },
        }),
        m(
          'pre',
          {
            style: {
              marginTop: '8px',
              fontSize: '11px',
              background: 'var(--pf-color-background-secondary)',
              padding: '8px',
              borderRadius: '4px',
            },
          },
          clickedNode ? `Clicked: ${clickedNode}` : 'Click a node to select it',
        ),
        clickedNode &&
          m(
            'button',
            {
              style: {marginTop: '8px', fontSize: '12px'},
              onclick: () => {
                clickedNode = undefined;
              },
            },
            'Clear selection',
          ),
      ]);
    },
  };
}

function SQLScatterChartDemo(): m.Component<{
  trace: Trace;
  height: number;
  showLegend: boolean;
  maxPoints: number;
  scaleAxes: boolean;
}> {
  let loader: SQLScatterChartLoader | undefined;

  return {
    view: ({attrs}) => {
      if (!loader) {
        loader = new SQLScatterChartLoader({
          engine: attrs.trace.engine,
          query: 'SELECT ts, dur, name FROM slice WHERE dur > 0 LIMIT 1000',
          xColumn: 'ts',
          yColumn: 'dur',
          seriesColumn: 'name',
        });
      }

      const config: ScatterChartLoaderConfig = {
        maxPoints: attrs.maxPoints,
      };
      const {data, isPending} = loader.use(config);

      return m('div', [
        m(Scatterplot, {
          data,
          height: attrs.height,
          xAxisLabel: 'Timestamp',
          yAxisLabel: 'Duration',
          showLegend: attrs.showLegend,
          scaleAxes: attrs.scaleAxes,
        }),
        m(
          'pre',
          {
            style: {
              marginTop: '8px',
              fontSize: '11px',
              background: 'var(--pf-color-background-secondary)',
              padding: '8px',
              borderRadius: '4px',
            },
          },
          [
            `query: 'SELECT ts, dur, name FROM slice WHERE dur > 0 LIMIT 1000'\n`,
            `xColumn: 'ts', yColumn: 'dur', seriesColumn: 'name'\n`,
            `loader.use(${JSON.stringify(config, null, 2)})`,
            isPending ? '\n(loading...)' : '',
          ],
        ),
      ]);
    },
    onremove: () => {
      loader?.dispose();
      loader = undefined;
    },
  };
}

function SQLTreemapDemo(): m.Component<{
  trace: Trace;
  height: number;
  showLabels: boolean;
  limit: number;
}> {
  let loader: SQLTreemapLoader | undefined;
  let clickedNode: string | undefined;

  return {
    view: ({attrs}) => {
      if (!loader) {
        loader = new SQLTreemapLoader({
          engine: attrs.trace.engine,
          query: 'SELECT name, dur, category FROM slice WHERE dur > 0',
          labelColumn: 'name',
          sizeColumn: 'dur',
          groupColumn: 'category',
        });
      }

      const config: TreemapLoaderConfig = {
        aggregation: 'SUM',
        limit: attrs.limit,
      };
      const {data, isPending} = loader.use(config);

      return m('div', [
        m(Treemap, {
          data,
          height: attrs.height,
          showLabels: attrs.showLabels,
          onNodeClick: (node) => {
            clickedNode = node.name;
          },
        }),
        m(
          'pre',
          {
            style: {
              marginTop: '8px',
              fontSize: '11px',
              background: 'var(--pf-color-background-secondary)',
              padding: '8px',
              borderRadius: '4px',
            },
          },
          [
            `query: 'SELECT name, dur, category FROM slice WHERE dur > 0'\n`,
            `labelColumn: 'name', sizeColumn: 'dur', groupColumn: 'category'\n`,
            `loader.use(${JSON.stringify(config, null, 2)})`,
            isPending ? '\n(loading...)' : '',
            clickedNode ? `\nClicked: ${clickedNode}` : '',
          ],
        ),
        clickedNode &&
          m(
            'button',
            {
              style: {marginTop: '8px', fontSize: '12px'},
              onclick: () => {
                clickedNode = undefined;
              },
            },
            'Clear selection',
          ),
      ]);
    },
    onremove: () => {
      loader?.dispose();
      loader = undefined;
    },
  };
}

// ---------------------------------------------------------------------------
// Static sample data for BoxplotChart demo
// ---------------------------------------------------------------------------

const BOXPLOT_SAMPLE_DATA: BoxplotData = {
  items: [
    {label: 'Chrome', min: 200, q1: 800, median: 1500, q3: 2300, max: 4200},
    {
      label: 'SurfaceFlinger',
      min: 400,
      q1: 500,
      median: 850,
      q3: 1050,
      max: 1200,
    },
    {label: 'SystemUI', min: 600, q1: 700, median: 2200, q3: 2650, max: 3100},
    {label: 'Launcher', min: 500, q1: 600, median: 1200, q3: 1500, max: 1800},
    {
      label: 'AudioFlinger',
      min: 800,
      q1: 900,
      median: 1000,
      q3: 1050,
      max: 1100,
    },
  ],
};

function BoxplotChartDemo(): m.Component<{
  height: number;
  horizontal: boolean;
}> {
  return {
    view: ({attrs}) => {
      return m('div', [
        m(BoxplotChart, {
          data: BOXPLOT_SAMPLE_DATA,
          height: attrs.height,
          categoryLabel: 'Process',
          valueLabel: 'Duration (ns)',
          orientation: attrs.horizontal ? 'horizontal' : 'vertical',
        }),
        m(
          'pre',
          {
            style: {
              marginTop: '8px',
              fontSize: '11px',
              background: 'var(--pf-color-background-secondary)',
              padding: '8px',
              borderRadius: '4px',
            },
          },
          'Static boxplot with quartile statistics per process',
        ),
      ]);
    },
  };
}

// ---------------------------------------------------------------------------
// Static sample data for HeatmapChart demo
// ---------------------------------------------------------------------------

const HEATMAP_SAMPLE_DATA: HeatmapData = (() => {
  const xLabels = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri'];
  const yLabels = ['Chrome', 'SurfaceFlinger', 'SystemUI', 'Launcher'];
  const rng = seededRandom(123);
  const values: Array<readonly [number, number, number]> = [];
  let min = Infinity;
  let max = -Infinity;
  for (let x = 0; x < xLabels.length; x++) {
    for (let y = 0; y < yLabels.length; y++) {
      const val = Math.floor(rng() * 100);
      min = Math.min(min, val);
      max = Math.max(max, val);
      values.push([x, y, val]);
    }
  }
  return {xLabels, yLabels, values, min, max};
})();

function HeatmapChartDemo(): m.Component<{
  height: number;
}> {
  return {
    view: ({attrs}) => {
      return m('div', [
        m(HeatmapChart, {
          data: HEATMAP_SAMPLE_DATA,
          height: attrs.height,
          xAxisLabel: 'Day',
          yAxisLabel: 'Process',
        }),
        m(
          'pre',
          {
            style: {
              marginTop: '8px',
              fontSize: '11px',
              background: 'var(--pf-color-background-secondary)',
              padding: '8px',
              borderRadius: '4px',
            },
          },
          'Static heatmap: process activity by day of week',
        ),
      ]);
    },
  };
}

// ---------------------------------------------------------------------------
// SQL CDF demo
// ---------------------------------------------------------------------------

function SQLCdfDemo(): m.Component<{
  trace: Trace;
  height: number;
  maxPoints: number;
  enableBrush: boolean;
}> {
  let loader: SQLCdfLoader | undefined;
  let xRange: {min: number; max: number} | undefined;

  return {
    view: ({attrs}) => {
      if (!loader) {
        loader = new SQLCdfLoader({
          engine: attrs.trace.engine,
          query: 'SELECT dur FROM slice WHERE dur > 0',
          valueColumn: 'dur',
        });
      }

      const config: CdfLoaderConfig = {
        maxPoints: attrs.maxPoints,
        filter: xRange,
      };
      const {data, isPending} = loader.use(config);

      return m('div', [
        m(LineChart, {
          data,
          height: attrs.height,
          xAxisLabel: 'Duration (ns)',
          yAxisLabel: 'Cumulative %',
          showPoints: false,
          scaleAxes: true,
          onBrush: attrs.enableBrush
            ? (range) => {
                xRange = {min: range.start, max: range.end};
              }
            : undefined,
        }),
        m(
          'pre',
          {
            style: {
              marginTop: '8px',
              fontSize: '11px',
              background: 'var(--pf-color-background-secondary)',
              padding: '8px',
              borderRadius: '4px',
            },
          },
          [
            `query: 'SELECT dur FROM slice WHERE dur > 0'\n`,
            `valueColumn: 'dur'\n`,
            `loader.use(${JSON.stringify(config, null, 2)})`,
            isPending ? '\n(loading...)' : '',
          ],
        ),
        xRange &&
          m(
            'button',
            {
              style: {marginTop: '8px', fontSize: '12px'},
              onclick: () => {
                xRange = undefined;
              },
            },
            'Clear filter',
          ),
      ]);
    },
    onremove: () => {
      loader?.dispose();
      loader = undefined;
    },
  };
}

// ---------------------------------------------------------------------------
// SQL Boxplot demo
// ---------------------------------------------------------------------------

function SQLBoxplotDemo(): m.Component<{
  trace: Trace;
  height: number;
  limit: number;
}> {
  let loader: SQLBoxplotLoader | undefined;

  return {
    view: ({attrs}) => {
      if (!loader) {
        loader = new SQLBoxplotLoader({
          engine: attrs.trace.engine,
          query: 'SELECT name, dur FROM slice WHERE dur > 0',
          categoryColumn: 'name',
          valueColumn: 'dur',
        });
      }

      const config: BoxplotLoaderConfig = {
        limit: attrs.limit,
      };
      const {data, isPending} = loader.use(config);

      return m('div', [
        m(BoxplotChart, {
          data,
          height: attrs.height,
          categoryLabel: 'Slice Name',
          valueLabel: 'Duration (ns)',
        }),
        m(
          'pre',
          {
            style: {
              marginTop: '8px',
              fontSize: '11px',
              background: 'var(--pf-color-background-secondary)',
              padding: '8px',
              borderRadius: '4px',
            },
          },
          [
            `query: 'SELECT name, dur FROM slice WHERE dur > 0'\n`,
            `categoryColumn: 'name', valueColumn: 'dur'\n`,
            `loader.use(${JSON.stringify(config, null, 2)})`,
            isPending ? '\n(loading...)' : '',
          ],
        ),
      ]);
    },
    onremove: () => {
      loader?.dispose();
      loader = undefined;
    },
  };
}

// ---------------------------------------------------------------------------
// SQL Heatmap demo
// ---------------------------------------------------------------------------

function SQLHeatmapDemo(): m.Component<{
  trace: Trace;
  height: number;
  xLimit: number;
  yLimit: number;
}> {
  let loader: SQLHeatmapLoader | undefined;

  return {
    view: ({attrs}) => {
      if (!loader) {
        loader = new SQLHeatmapLoader({
          engine: attrs.trace.engine,
          query: 'SELECT priority, end_state, dur FROM sched WHERE dur > 0',
          xColumn: 'priority',
          yColumn: 'end_state',
          valueColumn: 'dur',
        });
      }

      const config: HeatmapLoaderConfig = {
        aggregation: 'SUM',
        xLimit: attrs.xLimit,
        yLimit: attrs.yLimit,
      };
      const {data, isPending} = loader.use(config);

      return m('div', [
        m(HeatmapChart, {
          data,
          height: attrs.height,
          xAxisLabel: 'Priority',
          yAxisLabel: 'End State',
        }),
        m(
          'pre',
          {
            style: {
              marginTop: '8px',
              fontSize: '11px',
              background: 'var(--pf-color-background-secondary)',
              padding: '8px',
              borderRadius: '4px',
            },
          },
          [
            `query: 'SELECT priority, end_state, dur FROM sched WHERE dur > 0'\n`,
            `xColumn: 'priority', yColumn: 'end_state', valueColumn: 'dur'\n`,
            `loader.use(${JSON.stringify(config, null, 2)})`,
            isPending ? '\n(loading...)' : '',
          ],
        ),
      ]);
    },
    onremove: () => {
      loader?.dispose();
      loader = undefined;
    },
  };
}
