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
import {RecentQueriesSection} from '../query/recent_queries';
import {Icon} from '../../widgets/icon';
import {queryState} from '../query/query_state';
import {Card} from '../../widgets/card';
import {recentQueriesStorage} from '../query/recent_queries_storage';

interface HomePageAttrs {}

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
  view() {
    return m(
      '.pf-home-page',
      {
        style: {
          padding: '24px',
          overflowY: 'auto',
          height: '100%',
          display: 'flex',
          flexDirection: 'column',
          gap: '32px',
        },
      },
      m(
        '.pf-home-header',
        {style: {textAlign: 'center', maxWidth: '800px', margin: '0 auto'}},
        m(
          'h1',
          {
            style: {
              fontSize: '2.5rem',
              fontWeight: 'bold',
              marginBottom: '8px',
            },
          },
          'Welcome to BigTrace',
        ),
        m(
          'p',
          {
            style: {
              fontSize: '1.2rem',
              color: 'var(--pf-fg-secondary, #5f6368)',
            },
          },
          'Analyze traces at scale. BigTrace helps you find bugs and performance issues across thousands of traces.',
        ),
      ),

      m(
        'details',
        {
          open: recentQueriesStorage.data.length === 0,
          style: {maxWidth: '1000px', margin: '0 auto', width: '100%'},
        },
        m(
          'summary',
          {
            style: {
              fontSize: '1.5rem',
              fontWeight: '600',
              marginBottom: '16px',
              paddingBottom: '8px',
              cursor: 'pointer',
              borderBottom: '1px solid var(--pf-border-color)',
            },
          },
          'How to get started',
        ),
        m(
          'div',
          {
            style: {
              display: 'grid',
              gridTemplateColumns: 'repeat(auto-fit, minmax(300px, 1fr))',
              gap: '16px',
              marginTop: '16px',
            },
          },
          m(
            Card,
            m(
              'div',
              {
                style: {
                  display: 'flex',
                  alignItems: 'center',
                  gap: '8px',
                  marginBottom: '8px',
                },
              },
              m(Icon, {icon: 'settings', filled: true}),
              m('h3', {style: {margin: 0}}, '1. Configure Targets'),
            ),
            m(
              'p',
              {style: {margin: 0, color: 'var(--pf-fg-secondary)'}},
              'Define which traces you want to analyze in the ',
              m('a', {onclick: () => m.route.set('/settings')}, 'Settings'),
              ' page.',
            ),
          ),
          m(
            Card,
            m(
              'div',
              {
                style: {
                  display: 'flex',
                  alignItems: 'center',
                  gap: '8px',
                  marginBottom: '8px',
                },
              },
              m(Icon, {icon: 'edit', filled: true}),
              m('h3', {style: {margin: 0}}, '2. Execute Queries'),
            ),
            m(
              'p',
              {style: {margin: 0, color: 'var(--pf-fg-secondary)'}},
              'Write a PerfettoSQL query in the editor to run against your selected traces.',
            ),
          ),
          m(
            Card,
            m(
              'div',
              {
                style: {
                  display: 'flex',
                  alignItems: 'center',
                  gap: '8px',
                  marginBottom: '8px',
                },
              },
              m(Icon, {icon: 'analytics', filled: true}),
              m('h3', {style: {margin: 0}}, '3. Analyze Results'),
            ),
            m(
              'p',
              {style: {margin: 0, color: 'var(--pf-fg-secondary)'}},
              'Execute the query to see aggregated data and insights in the results grid.',
            ),
          ),
        ),
      ),

      m(
        'details',
        {
          open: recentQueriesStorage.data.length === 0,
          style: {maxWidth: '1000px', margin: '0 auto', width: '100%'},
        },
        m(
          'summary',
          {
            style: {
              fontSize: '1.5rem',
              fontWeight: '600',
              marginBottom: '16px',
              paddingBottom: '8px',
              cursor: 'pointer',
              borderBottom: '1px solid var(--pf-border-color)',
            },
          },
          'Examples',
        ),
        m(
          'div',
          {
            style: {
              display: 'grid',
              gridTemplateColumns: 'repeat(auto-fit, minmax(400px, 1fr))',
              gap: '16px',
              marginTop: '16px',
            },
          },
          m(
            Card,
            {
              interactive: true,
              onclick: () => {
                queryState.initialQuery = SLICE_COUNT_QUERY;
                m.route.set('/query');
              },
            },
            m(
              'div',
              {
                style: {
                  display: 'flex',
                  alignItems: 'center',
                  gap: '8px',
                  marginBottom: '8px',
                },
              },
              m(Icon, {icon: 'search'}),
              m('h3', {style: {margin: 0}}, 'Slice Count'),
            ),
            m(
              'p',
              {style: {margin: '0 0 16px 0', color: 'var(--pf-fg-secondary)'}},
              'Count the total number of slices in the trace.',
            ),
            m(
              'pre',
              {
                style: {
                  padding: '16px',
                  background: 'var(--pf-bg-secondary)',
                  borderRadius: '4px',
                  overflowX: 'auto',
                  margin: 0,
                  fontSize: '0.9rem',
                },
              },
              SLICE_COUNT_QUERY,
            ),
          ),
          m(
            Card,
            {
              interactive: true,
              onclick: () => {
                queryState.initialQuery = CPU_TIME_QUERY;
                m.route.set('/query');
              },
            },
            m(
              'div',
              {
                style: {
                  display: 'flex',
                  alignItems: 'center',
                  gap: '8px',
                  marginBottom: '8px',
                },
              },
              m(Icon, {icon: 'timer'}),
              m('h3', {style: {margin: 0}}, 'Top CPU Consumers'),
            ),
            m(
              'p',
              {style: {margin: '0 0 16px 0', color: 'var(--pf-fg-secondary)'}},
              'Find the processes using the most CPU time.',
            ),
            m(
              'pre',
              {
                style: {
                  padding: '16px',
                  background: 'var(--pf-bg-secondary)',
                  borderRadius: '4px',
                  overflowX: 'auto',
                  margin: 0,
                  fontSize: '0.9rem',
                },
              },
              CPU_TIME_QUERY,
            ),
          ),
        ),
      ),

      m(
        'details',
        {
          open: recentQueriesStorage.data.length > 0,
          style: {maxWidth: '1000px', margin: '0 auto', width: '100%'},
        },
        m(
          'summary',
          {
            style: {
              fontSize: '1.5rem',
              fontWeight: '600',
              paddingBottom: '8px',
              cursor: 'pointer',
              borderBottom: '1px solid var(--pf-border-color)',
            },
          },
          'Recent Queries',
        ),
        m(
          'div',
          {style: {marginTop: '16px'}},
          m(RecentQueriesSection, {
            onLoadQuery: (query: string) => {
              queryState.initialQuery = query;
              m.route.set('/query');
            },
          }),
        ),
      ),
    );
  }
}
