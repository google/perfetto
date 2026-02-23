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
import {Histogram} from '../../../components/widgets/charts/histogram';
import {
  InMemoryHistogramLoader,
  SQLHistogramLoader,
  HistogramLoaderConfig,
} from '../../../components/widgets/charts/histogram_loader';
import {App} from '../../../public/app';
import {renderWidgetShowcase} from '../widgets_page_utils';
import {Trace} from '../../../public/trace';

// Generate sample data with normal distribution
function generateNormalData(
  count: number,
  mean: number,
  stdDev: number,
): number[] {
  const data: number[] = [];
  for (let i = 0; i < count; i++) {
    // Box-Muller transform for normal distribution
    const u1 = Math.random();
    const u2 = Math.random();
    const z = Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
    data.push(mean + z * stdDev);
  }
  return data;
}

export function renderHistogram(app: App): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Histogram'),
      m('p', [
        'Histogram is a pure SVG-based chart component for visualizing the distribution of numeric data. ',
        'It receives pre-computed histogram data and renders it, with callbacks for user interactions.',
      ]),
      m('p', [
        'The component is completely unopinionated about data loading. Use the computeHistogram() ',
        'utility function to convert raw values to histogram data, or use the histogram loaders for ',
        'caching and async SQL queries.',
      ]),
    ),

    m('h2', 'InMemoryHistogramLoader'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(HistogramDemo, {
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

    // SQL loader demo (only shown when a trace is loaded)
    app.trace && m('h2', {style: {marginTop: '32px'}}, 'SQLHistogramLoader'),
    app.trace &&
      renderWidgetShowcase({
        renderWidget: (opts) => {
          return m(SQLHistogramDemo, {
            trace: app.trace!,
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
    !app.trace &&
      m(
        'p',
        {style: {marginTop: '32px', color: 'var(--pf-color-text-muted)'}},
        'Load a trace to see the SQLHistogramLoader demo.',
      ),
  ];
}

function HistogramDemo(): m.Component<{
  bucketCount: number;
  height: number;
  enableBrush: boolean;
  logScale: boolean;
}> {
  // Pre-generate sample dataset
  const normalData = generateNormalData(1000, 50, 15);

  // Shared loader instance for the showcase
  const showcaseLoader = new InMemoryHistogramLoader(normalData);

  // Track filter state for the showcase
  let showcaseFilter: {min: number; max: number} | undefined;

  return {
    view: ({attrs}) => {
      const config: HistogramLoaderConfig = {
        bucketCount: attrs.bucketCount,
        filter: showcaseFilter,
      };
      const {data} = showcaseLoader.use(config);
      return m('div', [
        m(Histogram, {
          data,
          height: attrs.height,
          xAxisLabel: 'Value',
          yAxisLabel: 'Count',
          logScale: attrs.logScale,
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
