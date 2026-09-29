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
import {DualMetricAxis, DualMetricBar} from '../../../widgets/dual_metric_bar';
import {KpiCard, KpiCardGroup} from '../../../widgets/kpi_card';
import {TimelineSparkline} from '../../../widgets/timeline_sparkline';
import {renderWidgetShowcase} from '../widgets_page_utils';

let demoFrame = 4;

export function renderKpiCardDemo(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'KpiCard, DualMetricBar & TimelineSparkline'),
      m(
        'p',
        'Compact metric cards, dual horizontal CPU/memory bars, and stacked-area + line sparklines for bucketed timeline visualizations.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: ({active, wide}) =>
        m(
          'div',
          {style: {display: 'flex', flexDirection: 'column', gap: '12px'}},
          [
            m(KpiCardGroup, [
              m(KpiCard, {
                value: 6,
                label: 'starting',
                swatchColor: '#e9a23b',
                active,
              }),
              m(KpiCard, {
                value: 14,
                label: 'receiving',
                swatchColor: '#3b7ddd',
              }),
              m(KpiCard, {
                value: 18,
                label: 'running CPU',
                swatchColor: '#2fa36b',
              }),
              m(KpiCard, {
                value: '1.42 GB',
                label: "receivers' anon + swap (184 MB swap)",
                swatchColor: '#6f42c1',
                swatchCircular: true,
                valueColor: '#4c2c92',
                wide,
              }),
            ]),
            m(
              'div',
              {style: {width: '320px'}},
              m(DualMetricAxis, {
                topMax: 100,
                topUnit: 'ms',
                bottomMax: 500,
                bottomUnit: 'MB',
              }),
              m(DualMetricBar, {
                topValue: 72,
                topMax: 100,
                topColor: '#2fa36b',
                bottomSecondaryValue: 260,
                bottomPrimaryValue: 140,
                bottomOverflowValue: 45,
                bottomMax: 500,
              }),
            ),
            m(TimelineSparkline, {
              numFrames: 12,
              currentFrame: demoFrame,
              zeroFrameIndex: 2,
              bands: [
                {
                  key: 'I',
                  color: '#a4acb7',
                  values: [2, 3, 5, 8, 12, 15, 18, 20, 22, 24, 25, 26],
                },
                {
                  key: 'C',
                  color: '#2fa36b',
                  values: [0, 1, 4, 9, 11, 10, 8, 6, 5, 4, 3, 2],
                },
                {
                  key: 'R',
                  color: '#3b7ddd',
                  values: [0, 0, 6, 8, 5, 3, 2, 1, 1, 0, 0, 0],
                },
              ],
              lineSeries: {
                color: '#6f42c1',
                values: [
                  180, 220, 340, 520, 740, 890, 960, 1020, 1080, 1100, 1110,
                  1120,
                ],
              },
              onSelectFrame: (idx) => {
                demoFrame = idx;
              },
            }),
          ],
        ),
      initialOpts: {active: true, wide: true},
    }),
  ];
}
