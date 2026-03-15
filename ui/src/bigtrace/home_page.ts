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
import {RecentQueriesSection} from './recent_queries';
import {Card, CardStack} from '../widgets/card';
import {queryState} from './query_state';

interface HomePageAttrs {
  navigateTo: (page: string) => void;
}

const SLICE_COUNT_QUERY = `SELECT
  COUNT(*) as slice_count
FROM slice`;

const CPU_TIME_QUERY = `SELECT
  p.name,
  sum(s.dur)/1e9 as cpu_sec
FROM sched s
JOIN thread t USING (utid)
JOIN process p USING (upid)
GROUP BY p.name
ORDER BY cpu_sec DESC
LIMIT 10`;

export class HomePage implements m.ClassComponent<HomePageAttrs> {
  view({attrs}: m.Vnode<HomePageAttrs>) {
    return m(
        '.page',
        {style: {padding: '2em', overflowY: 'auto', height: '100%'}},
        m('.page-title', {style: {textAlign: 'center'}}, m('h1', 'Welcome to BigTrace')),
        m('p', 'Analyze traces at scale 🚀. BigTrace helps you find bugs 🐛 and performance issues 🐢 across thousands of traces.'),
        
        m('.quick-start', {style: {marginBottom: '2em'}},
          m('.pf-nav-section-header', m('span', 'How to get started')),
          m(CardStack,
            m(Card,
              m('h3', '1. Write a query'),
              m('p', 'Use the query editor to write a PerfettoSQL query.'),
            ),
            m(Card,
              m('h3', '2. Run it'),
              m('p', 'Click "Run Query" or press Cmd/Ctrl + Enter.'),
            ),
            m(Card,
              m('h3', '3. Analyze'),
              m('p', 'Browse the results in the table below.'),
            ),
          ),
        ),
        
        m('.quick-links', {style: {marginBottom: '2em'}},
          m('.pf-nav-section-header', m('span', 'Examples')),
          m(CardStack,
            m(Card, {
              interactive: true,
              onclick: () => {
                queryState.initialQuery = SLICE_COUNT_QUERY;
                attrs.navigateTo('bigtrace');
              },
            }, m('pre', {style: {maxHeight: '100px', overflowY: 'auto', whiteSpace: 'pre-wrap'}}, SLICE_COUNT_QUERY)),
            m(Card, {
              interactive: true,
              onclick: () => {
                queryState.initialQuery = CPU_TIME_QUERY;
                attrs.navigateTo('bigtrace');
              },
            }, m('pre', {style: {maxHeight: '100px', overflowY: 'auto', whiteSpace: 'pre-wrap'}}, CPU_TIME_QUERY)),
            
          ),
        ),
        m(RecentQueriesSection, {
          onLoadQuery: (query: string) => {
            queryState.initialQuery = query;
            attrs.navigateTo('bigtrace');
          },
        }),
    );
  }
}
