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
import {
  AggregationType,
  isIntegerAggregation,
} from '../../../components/widgets/charts/chart_utils';
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
        'Pure SVG-based chart components for visualizing data. ',
        'Includes BarChart, LineChart, PieChart, and Histogram.',
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
          aggregation: opts.aggregation as AggregationType,
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
          'COUNT',
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
          aggregation: opts.aggregation as AggregationType,
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
          'COUNT',
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
        });
      },
      initialOpts: {
        height: 250,
        enableBrush: true,
        showPoints: true,
        maxPoints: 200,
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
          aggregation: opts.aggregation as AggregationType,
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
          'COUNT',
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
  aggregation: AggregationType;
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

      const measureLabels: Record<AggregationType, string> = {
        SUM: 'Total Duration',
        AVG: 'Avg Duration',
        MIN: 'Min Duration',
        MAX: 'Max Duration',
        COUNT: 'Slice Count',
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
  aggregation: AggregationType;
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

      const measureLabels: Record<AggregationType, string> = {
        SUM: 'Total Duration',
        AVG: 'Avg Duration',
        MIN: 'Min Duration',
        MAX: 'Max Duration',
        COUNT: 'Slice Count',
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
  aggregation: AggregationType;
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

  return {
    view: ({attrs}) => {
      const fullData = attrs.multiSeries
        ? LINE_CHART_MULTI_SERIES_DATA
        : LINE_CHART_SAMPLE_DATA;

      // Filter data to the brushed X range
      const range = brushRange;
      const data: LineChartData =
        range !== undefined
          ? {
              series: fullData.series.map((s) => ({
                ...s,
                points: s.points.filter(
                  (p) => p.x >= range.start && p.x <= range.end,
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
          onBrush: attrs.enableBrush
            ? (range) => {
                brushRange = range;
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
