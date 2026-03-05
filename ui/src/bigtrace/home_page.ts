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

export class HomePage implements m.ClassComponent<HomePageAttrs> {
  view({attrs}: m.Vnode<HomePageAttrs>) {
    return m(
        '.page',
        {style: {padding: '2em'}},
        m('.page-title', m('h1', 'Welcome to BigTrace')),
        m('p', 'BigTrace is a tool to run queries on multiple traces.'),
        m('.quick-links', {style: {marginBottom: '2em'}},
          m('.pf-nav-section-header', m('span', 'Examples')),
          m(CardStack,
            m(Card, {
              interactive: true,
              onclick: () => {
                queryState.initialQuery = 'SELECT * FROM slice LIMIT 10';
                attrs.navigateTo('bigtrace');
              },
            }, m('h3', 'SELECT * FROM slice LIMIT 10')),
            m(Card, {
              interactive: true,
              onclick: () => {
                queryState.initialQuery = 'SELECT * FROM process LIMIT 10';
                attrs.navigateTo('bigtrace');
              },
            }, m('h3', 'SELECT * FROM process LIMIT 10')),
            
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
