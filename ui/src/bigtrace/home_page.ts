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
import {assetSrc} from '../base/assets';
import {AppImpl} from '../core/app_impl';
import {Switch} from '../widgets/switch';

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
    const themeSetting = AppImpl.instance.settings.get<string>('theme');
    const isDarkMode = themeSetting?.get() === 'dark';

    return m(
      '.pf-home-page',
      m(
        '.pf-home-page__center',
        m(
          '.pf-home-page__title',
          m(`img.logo[src=${assetSrc('assets/logo-3d.png')}]`),
          'BigTrace',
        ),
        m(
          'p',
          {style: {color: 'var(--pf-color-text-muted)', fontSize: '1.2em', margin: '0 0 32px 0', textAlign: 'center'}},
          'Analyze traces at scale. BigTrace helps you find bugs and performance issues across thousands of traces.'
        ),
        m(
          '.pf-home-page__hints',
          
          m(
            '.pf-home-page__section',
            m('.pf-home-page__section-title', 'How to get started'),
            m(
              '.pf-home-page__section-content',
              m('.pf-home-page__shortcut', {style: {justifyContent: 'flex-start'}},
                m('strong', '1.'), m('span.pf-home-page__shortcut-label', {style: {marginLeft: '12px'}}, 'Write a PerfettoSQL query in the editor.')
              ),
              m('.pf-home-page__shortcut', {style: {justifyContent: 'flex-start'}},
                m('strong', '2.'), m('span.pf-home-page__shortcut-label', {style: {marginLeft: '12px'}}, 'Click "Run Query" or press Cmd/Ctrl + Enter.')
              ),
              m('.pf-home-page__shortcut', {style: {justifyContent: 'flex-start'}},
                m('strong', '3.'), m('span.pf-home-page__shortcut-label', {style: {marginLeft: '12px'}}, 'Analyze your results across multiple traces.')
              )
            )
          ),
          
          m(
            '.pf-home-page__section',
            m('.pf-home-page__section-title', 'Examples'),
            m(
              '.pf-home-page__section-content',
              m(CardStack,
                m(Card, {
                  interactive: true,
                  onclick: () => {
                    queryState.initialQuery = SLICE_COUNT_QUERY;
                    attrs.navigateTo('bigtrace');
                  },
                }, m('h3', {style: {margin: '0 0 8px 0'}}, 'Slice Count'),
                   m('p', {style: {margin: '0 0 8px 0', color: 'var(--pf-color-text-muted)'}}, 'Count the total number of slices in the trace.'),
                   m('pre', {style: {maxHeight: '100px', overflowY: 'auto', whiteSpace: 'pre-wrap', margin: '0'}}, SLICE_COUNT_QUERY)),
                m(Card, {
                  interactive: true,
                  onclick: () => {
                    queryState.initialQuery = CPU_TIME_QUERY;
                    attrs.navigateTo('bigtrace');
                  },
                }, m('h3', {style: {margin: '0 0 8px 0'}}, 'Top CPU Consumers'),
                   m('p', {style: {margin: '0 0 8px 0', color: 'var(--pf-color-text-muted)'}}, 'Find the processes using the most CPU time.'),
                   m('pre', {style: {maxHeight: '100px', overflowY: 'auto', whiteSpace: 'pre-wrap', margin: '0'}}, CPU_TIME_QUERY)),
              ),
            ),
          ),
          
          m(
            '.pf-home-page__section',
            m('.pf-home-page__section-title', 'Recent Queries'),
            m(
              '.pf-home-page__section-content',
              m(RecentQueriesSection, {
                onLoadQuery: (query: string) => {
                  queryState.initialQuery = query;
                  attrs.navigateTo('bigtrace');
                },
              })
            )
          )
        ),
        m(
          '.pf-home-page__links',
          m(Switch, {
            label: 'Dark mode',
            checked: isDarkMode,
            onchange: (e: Event) => {
              themeSetting?.set(
                (e.target as HTMLInputElement).checked ? 'dark' : 'light',
              );
            },
          }),
        ),
      ),
    );
  }
}
