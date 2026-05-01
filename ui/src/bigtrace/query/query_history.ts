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
import {Icons} from '../../base/semantic_icons';
import {Button} from '../../widgets/button';
import {Stack} from '../../widgets/stack';
import {queryHistoryStorage, QueryHistoryEntry} from './query_history_storage';

interface QueryHistoryComponentAttrs {
  readonly className?: string;
  runQuery: (query: string) => void;
  setQuery: (query: string) => void;
}

export class QueryHistoryComponent
  implements m.ClassComponent<QueryHistoryComponentAttrs>
{
  view({attrs}: m.CVnode<QueryHistoryComponentAttrs>) {
    const {runQuery, setQuery, ...rest} = attrs;
    const unstarred: HistoryItemAttrs[] = [];
    const starred: HistoryItemAttrs[] = [];
    for (let i = 0; i < queryHistoryStorage.data.length; i++) {
      const entry = queryHistoryStorage.data[i];
      const arr = entry.starred ? starred : unstarred;
      arr.push({index: i, entry, runQuery, setQuery});
    }
    return m(
      '.pf-query-history',
      {
        ...rest,
      },
      m(
        '.pf-query-history__header',
        `Query history (${queryHistoryStorage.data.length} queries)`,
      ),
      starred.map((a) => m(HistoryItemComponent, a)),
      unstarred.map((a) => m(HistoryItemComponent, a)),
    );
  }
}

interface HistoryItemAttrs {
  index: number;
  entry: QueryHistoryEntry;
  runQuery: (query: string) => void;
  setQuery: (query: string) => void;
}

class HistoryItemComponent implements m.ClassComponent<HistoryItemAttrs> {
  view(vnode: m.Vnode<HistoryItemAttrs>): m.Child {
    const query = vnode.attrs.entry.query;
    return m(
      '.pf-query-history__item',
      m(
        Stack,
        {
          className: 'pf-query-history__item-buttons',
          orientation: 'horizontal',
        },
        [
          m(Button, {
            onclick: () => {
              queryHistoryStorage.setStarred(
                vnode.attrs.index,
                !vnode.attrs.entry.starred,
              );
            },
            icon: Icons.Star,
            iconFilled: vnode.attrs.entry.starred,
          }),
          m(Button, {
            onclick: () => vnode.attrs.setQuery(query),
            icon: Icons.Edit,
          }),
          m(Button, {
            onclick: () => vnode.attrs.runQuery(query),
            icon: Icons.Play,
          }),
          m(Button, {
            onclick: () => {
              queryHistoryStorage.remove(vnode.attrs.index);
            },
            icon: Icons.Delete,
          }),
        ],
      ),
      m(
        'pre',
        {
          onclick: () => vnode.attrs.setQuery(query),
          ondblclick: () => vnode.attrs.runQuery(query),
        },
        query,
      ),
    );
  }
}
