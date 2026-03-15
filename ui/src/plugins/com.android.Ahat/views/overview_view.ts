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
import type {OverviewData} from '../types';
import {fmtSize} from '../format';
import type {NavFn} from '../components';

interface OverviewViewAttrs {
  overview: OverviewData;
  name: string;
  navigate: NavFn;
}
function OverviewView(): m.Component<OverviewViewAttrs> {
  return {
    view(vnode) {
      const {overview, name, navigate} = vnode.attrs;
      const heapIndices: number[] = [];
      for (let i = 0; i < overview.heaps.length; i++) {
        const h = overview.heaps[i];
        if (h.java + h.native_ > 0) {
          heapIndices.push(i);
        }
      }
      const heaps = heapIndices.map((i) => overview.heaps[i]);
      const totalJava = heaps.reduce((a, h) => a + h.java, 0);
      const totalNative = heaps.reduce((a, h) => a + h.native_, 0);

      return m('div', [
        m('h2', {class: 'ah-view-heading'}, 'Overview'),

        m('div', {class: 'ah-card ah-mb-4'}, [
          m('h3', {class: 'ah-sub-heading'}, 'General Information'),
          m('div', {class: 'ah-info-grid--wide'}, [
            m('span', {class: 'ah-info-grid__label'}, 'Heap Dump:'),
            m('span', name),
            m('span', {class: 'ah-info-grid__label'}, 'Total Instances:'),
            m(
              'span',
              {class: 'ah-mono'},
              overview.instanceCount.toLocaleString(),
            ),
            m('span', {class: 'ah-info-grid__label'}, 'Heaps:'),
            m('span', heaps.map((h) => h.name).join(', ')),
          ]),
        ]),
        m('div', {class: 'ah-card'}, [
          m('h3', {class: 'ah-sub-heading'}, 'Bytes Retained by Heap'),
          m('table', {class: 'ah-overview-table'}, [
            m('thead', [
              m('tr', [
                m('th', {class: 'ah-overview-th'}, 'Heap'),
                m('th', {class: 'ah-overview-th--right'}, 'Java Size'),
                m('th', {class: 'ah-overview-th--right'}, 'Native Size'),
                m('th', {class: 'ah-overview-th--right'}, 'Total Size'),
              ]),
            ]),
            m('tbody', [
              m('tr', {class: 'ah-overview-total'}, [
                m('td', {class: 'ah-overview-td'}, 'Total'),
                m('td', {class: 'ah-overview-td--right'}, fmtSize(totalJava)),
                m('td', {class: 'ah-overview-td--right'}, fmtSize(totalNative)),
                m(
                  'td',
                  {class: 'ah-overview-td--right'},
                  fmtSize(totalJava + totalNative),
                ),
              ]),
              heapIndices.map((i) => {
                const h = overview.heaps[i];
                return m('tr', {key: h.name, class: 'ah-overview-row'}, [
                  m('td', {class: 'ah-overview-td'}, h.name),
                  m('td', {class: 'ah-overview-td--right'}, fmtSize(h.java)),
                  m('td', {class: 'ah-overview-td--right'}, fmtSize(h.native_)),
                  m(
                    'td',
                    {class: 'ah-overview-td--total'},
                    fmtSize(h.java + h.native_),
                  ),
                ]);
              }),
            ]),
          ]),
        ]),
        overview.duplicateBitmaps && overview.duplicateBitmaps.length > 0
          ? m('div', {class: 'ah-card ah-mt-4'}, [
              m('h3', {class: 'ah-sub-heading'}, 'Heap Analysis Results'),
              m(
                'p',
                {
                  style: {
                    fontSize: '0.875rem',
                    lineHeight: '1.25rem',
                    color: 'var(--ah-text-secondary)',
                    marginBottom: '0.5rem',
                  },
                },
                [
                  overview.duplicateBitmaps.length +
                    ' group' +
                    (overview.duplicateBitmaps.length > 1 ? 's' : '') +
                    ' of duplicate bitmaps detected, wasting ',
                  m(
                    'span',
                    {class: 'ah-mono ah-semibold'},
                    fmtSize(
                      overview.duplicateBitmaps.reduce(
                        (a, g) => a + g.wastedBytes,
                        0,
                      ),
                    ),
                  ),
                  '. ',
                  m(
                    'button',
                    {
                      class: 'ah-link--alt',
                      onclick: () => navigate('bitmaps'),
                    },
                    'View Bitmaps',
                  ),
                ],
              ),
              m(
                'table',
                {
                  class: 'ah-overview-table',
                  style: {fontSize: '0.875rem', lineHeight: '1.25rem'},
                },
                [
                  m('thead', [
                    m('tr', [
                      m('th', {class: 'ah-overview-th'}, 'Dimensions'),
                      m('th', {class: 'ah-overview-th--right'}, 'Copies'),
                      m('th', {class: 'ah-overview-th--right'}, 'Total'),
                      m('th', {class: 'ah-overview-th--right'}, 'Wasted'),
                    ]),
                  ]),
                  m(
                    'tbody',
                    overview.duplicateBitmaps.map((g, i) =>
                      m('tr', {key: i, class: 'ah-overview-row'}, [
                        m(
                          'td',
                          {class: 'ah-overview-td ah-mono'},
                          g.width + ' \u00d7 ' + g.height,
                        ),
                        m(
                          'td',
                          {class: 'ah-overview-td--right'},
                          String(g.count),
                        ),
                        m(
                          'td',
                          {class: 'ah-overview-td--right'},
                          fmtSize(g.totalBytes),
                        ),
                        m(
                          'td',
                          {class: 'ah-overview-td--right ah-delta-pos'},
                          fmtSize(g.wastedBytes),
                        ),
                      ]),
                    ),
                  ),
                ],
              ),
            ])
          : null,
      ]);
    },
  };
}

export default OverviewView;
