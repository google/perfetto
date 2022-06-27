// Copyright (C) 2020 The Android Open Source Project
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

import * as m from 'mithril';

import {Actions} from '../common/actions';
import {globals} from './globals';
import {createPage} from './pages';

function getCurrSelectedMetric() {
  const {availableMetrics, selectedIndex} = globals.state.metrics;
  if (!availableMetrics) return undefined;
  if (selectedIndex === undefined) return undefined;
  return availableMetrics[selectedIndex];
}

class MetricResult implements m.ClassComponent {
  view() {
    const metricResult = globals.metricResult;
    if (metricResult === undefined) return undefined;
    const currSelection = getCurrSelectedMetric();
    if (!(metricResult && metricResult.name === currSelection)) {
      return undefined;
    }
    if (metricResult.error !== undefined) {
      return m('pre.metric-error', metricResult.error);
    }
    if (metricResult.resultString !== undefined) {
      return m('pre', metricResult.resultString);
    }
    return undefined;
  }
}

class MetricPicker implements m.ClassComponent {
  view() {
    const {availableMetrics, selectedIndex} = globals.state.metrics;
    if (availableMetrics === undefined) return 'Loading metrics...';
    if (availableMetrics.length === 0) return 'No metrics available';
    if (selectedIndex === undefined) {
      throw Error('Should not happen when avaibleMetrics is non-empty');
    }

    return m('div', [
      'Select a metric:',
      m('select',
        {
          selectedIndex: globals.state.metrics.selectedIndex,
          onchange: (e: InputEvent) => {
            globals.dispatch(Actions.setMetricSelectedIndex(
                {index: (e.target as HTMLSelectElement).selectedIndex}));
          },
        },
        availableMetrics.map(
            (metric) => m('option', {value: metric, key: metric}, metric))),
      m('button.metric-run-button',
        {onclick: () => globals.dispatch(Actions.requestSelectedMetric({}))},
        'Run'),
    ]);
  }
}

export const MetricsPage = createPage({
  view() {
    return m(
        '.metrics-page',
        m(MetricPicker),
        m(MetricResult),
    );
  },
});
