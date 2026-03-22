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
import {Chart} from './charts/d3/charts/chart';
import {BaseRenderer} from './charts/d3/charts/base_renderer';
import {
  FilterSelectionStrategy,
  OpacitySelectionStrategy,
  HistogramSelectionStrategy,
} from './charts/d3/charts/selection';
import {RENDERERS} from './charts/d3/charts/registry';
import {ChartSpec, ChartType} from './charts/d3/data/types';
import {Spinner} from './spinner';

export interface ChartWidgetAttrs {
  chart: Chart;
  onRemove?: () => void;
  onDuplicate?: () => void;
  cleanup?: () => void;
}

export const ChartWidget: m.Component<ChartWidgetAttrs> = {
  oncreate({dom, attrs}) {
    const svg = dom.querySelector('svg') as SVGElement;
    const chart = attrs.chart;
    const rendererFactory = RENDERERS[chart.spec.type];

    if (rendererFactory === undefined) {
      console.error(`No renderer for chart type: ${chart.spec.type}`);
      return;
    }

    // Create a new renderer instance for this chart to avoid callback collision
    const renderer = rendererFactory();

    // Setup filter request callback
    renderer.onFilterRequest = (filters) => {
      if (filters.length === 0) {
        chart.clearChartFilters();
      } else {
        filters.forEach((f) => chart.addPendingFilter(f.col, f.op, f.val));
      }
    };

    const render = () => {
      // Update strategy based on settings
      if (renderer instanceof BaseRenderer) {
        const updateSourceChart = chart.getFilterStore().getUpdateSourceChart();
        const isHistogram = chart.spec.type === ChartType.Histogram;

        let strategy;
        if (updateSourceChart) {
          strategy = new FilterSelectionStrategy();
        } else {
          strategy = isHistogram
            ? new HistogramSelectionStrategy()
            : new OpacitySelectionStrategy();
        }
        renderer.setSelectionStrategy(strategy);
      }

      renderer.render(svg, chart.getData(), chart.spec);
    };

    // Resize Observer for robust responsiveness
    const observer = new ResizeObserver(() => {
      render();
    });
    observer.observe(dom);

    // Subscribe to data changes
    chart.onDataChange = () => {
      render();
      m.redraw();
    };

    chart.onFilterStateChange = () => {
      m.redraw();
    };

    // Subscribe to settings changes to update strategy
    const unsubSettings = chart.getFilterStore().subscribeToSettings(() => {
      render();
    });

    // Initial render
    render();

    // Store cleanup function
    attrs.cleanup = () => {
      observer.disconnect();
      unsubSettings();
      chart.onDataChange = undefined;
      chart.onFilterStateChange = undefined;
    };
  },

  onremove({attrs}) {
    if (attrs.cleanup) attrs.cleanup();
    attrs.chart.destroy();
  },

  view({attrs}) {
    const {chart} = attrs;
    const title = getChartTitle(chart.spec);
    const hasActiveFilter = chart.hasActiveFilters();

    return m('.chart-container', [
      m(`.chart-header${hasActiveFilter ? '.has-active-filter' : ''}`, [
        m('span.chart-title', title),
        m('.chart-actions', [
          attrs.onDuplicate !== undefined &&
            m(
              'button',
              {
                onclick: attrs.onDuplicate,
              },
              'Duplicate',
            ),
          attrs.onRemove !== undefined &&
            m(
              'button',
              {
                onclick: attrs.onRemove,
              },
              '×',
            ),
        ]),
      ]),
      m('svg.chart-canvas'),
      chart.isLoading() && m('.chart-loading', m(Spinner)),
    ]);
  },
};

function getChartTitle(spec: ChartSpec): string {
  switch (spec.type) {
    case ChartType.Bar:
      return `${spec.y} by ${spec.x}`;
    case ChartType.Histogram:
      return `Histogram: ${spec.x}`;
    case ChartType.Cdf:
      return `CDF: ${spec.x}`;
    case ChartType.Scatter:
      return `${spec.y} vs ${spec.x}`;
    case ChartType.Boxplot:
      return `Boxplot: ${spec.y} by ${spec.x}`;
    case ChartType.Heatmap:
      return `Heatmap: ${spec.value} by ${spec.x} × ${spec.y}`;
    case ChartType.Line:
      return `Line: ${spec.y} vs ${spec.x}`;
    case ChartType.Donut:
      return `Donut: ${spec.value} by ${spec.category}`;
    case ChartType.Violin:
      return `Violin: ${spec.y} by ${spec.x}`;
    default:
      return '';
  }
}
