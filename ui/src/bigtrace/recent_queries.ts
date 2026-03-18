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
import {Card, CardStack} from '../widgets/card';
import {recentQueriesStorage, RecentQueryEntry} from './recent_queries_storage';

export interface RecentQueriesSectionAttrs {
  onLoadQuery: (query: string) => void;
}

class RecentQueryCard implements m.ClassComponent<{entry: RecentQueryEntry, onLoadQuery: (query: string) => void}> {
  view({attrs}: m.CVnode<{entry: RecentQueryEntry, onLoadQuery: (query: string) => void}>) {
    const {entry, onLoadQuery} = attrs;
    return m(
        Card,
        {
          interactive: true,
          onclick: () => onLoadQuery(entry.query),
        },
        m('p', new Date(entry.timestamp).toLocaleString()),
        m('pre', {style: {maxHeight: '100px', overflowY: 'auto', whiteSpace: 'pre-wrap'}}, entry.query),
    );
  }
}

export class RecentQueriesSection implements
    m.ClassComponent<RecentQueriesSectionAttrs> {
  view({attrs}: m.CVnode<RecentQueriesSectionAttrs>) {
    const queries = recentQueriesStorage.data;

    return m(
        '.pf-recent-queries-section',
        {style: {width: '100%'}},
        m('.pf-nav-section-header', m('span', 'Recent Queries')),
        queries.length > 0 ?
            m(CardStack, {style: {width: '100%'}},
              queries.map(
                  (entry) => m(
                      RecentQueryCard,
                      {entry, onLoadQuery: attrs.onLoadQuery},
                      )),
              ) :
            m('.pf-recent-queries-empty', 'No recent queries'),
    );
  }
}
