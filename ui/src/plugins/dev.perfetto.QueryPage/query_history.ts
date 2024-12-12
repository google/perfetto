// Copyright (C) 2022 The Android Open Source Project
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
import {assertTrue} from '../../base/logging';
import {Icon} from '../../widgets/icon';
import {z} from 'zod';
import {Trace} from '../../public/trace';

const QUERY_HISTORY_KEY = 'queryHistory';

export interface QueryHistoryComponentAttrs {
  trace: Trace;
  runQuery: (query: string) => void;
  setQuery: (query: string) => void;
}

export class QueryHistoryComponent
  implements m.ClassComponent<QueryHistoryComponentAttrs>
{
  view({attrs}: m.CVnode<QueryHistoryComponentAttrs>): m.Child {
    const runQuery = attrs.runQuery;
    const setQuery = attrs.setQuery;
    const unstarred: HistoryItemComponentAttrs[] = [];
    const starred: HistoryItemComponentAttrs[] = [];
    for (let i = queryHistoryStorage.data.length - 1; i >= 0; i--) {
      const entry = queryHistoryStorage.data[i];
      const arr = entry.starred ? starred : unstarred;
      arr.push({trace: attrs.trace, index: i, entry, runQuery, setQuery});
    }
    return m(
      '.query-history',
      m(
        'header.overview',
        `Query history (${queryHistoryStorage.data.length} queries)`,
      ),
      starred.map((attrs) => m(HistoryItemComponent, attrs)),
      unstarred.map((attrs) => m(HistoryItemComponent, attrs)),
    );
  }
}

export interface HistoryItemComponentAttrs {
  trace: Trace;
  index: number;
  entry: QueryHistoryEntry;
  runQuery: (query: string) => void;
  setQuery: (query: string) => void;
}

export class HistoryItemComponent
  implements m.ClassComponent<HistoryItemComponentAttrs>
{
  view(vnode: m.Vnode<HistoryItemComponentAttrs>): m.Child {
    const query = vnode.attrs.entry.query;
    return m(
      '.history-item',
      m(
        '.history-item-buttons',
        m(
          'button',
          {
            onclick: () => {
              queryHistoryStorage.setStarred(
                vnode.attrs.index,
                !vnode.attrs.entry.starred,
              );
              vnode.attrs.trace.scheduleFullRedraw();
            },
          },
          m(Icon, {icon: Icons.Star, filled: vnode.attrs.entry.starred}),
        ),
        m(
          'button',
          {
            onclick: () => vnode.attrs.setQuery(query),
          },
          m(Icon, {icon: 'edit'}),
        ),
        m(
          'button',
          {
            onclick: () => vnode.attrs.runQuery(query),
          },
          m(Icon, {icon: 'play_arrow'}),
        ),
        m(
          'button',
          {
            onclick: () => {
              queryHistoryStorage.remove(vnode.attrs.index);
              vnode.attrs.trace.scheduleFullRedraw();
            },
          },
          m(Icon, {icon: 'delete'}),
        ),
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

class HistoryStorage {
  data: QueryHistory;
  maxItems = 50;

  constructor() {
    this.data = this.load();
  }

  saveQuery(query: string) {
    const items = this.data;
    let firstUnstarred = -1;
    let countUnstarred = 0;
    for (let i = 0; i < items.length; i++) {
      if (!items[i].starred) {
        countUnstarred++;
        if (firstUnstarred === -1) {
          firstUnstarred = i;
        }
      }

      if (items[i].query === query) {
        // Query is already in the history, no need to save
        return;
      }
    }

    if (countUnstarred >= this.maxItems) {
      assertTrue(firstUnstarred !== -1);
      items.splice(firstUnstarred, 1);
    }

    items.push({query, starred: false});
    this.save();
  }

  setStarred(index: number, starred: boolean) {
    assertTrue(index >= 0 && index < this.data.length);
    this.data[index].starred = starred;
    this.save();
  }

  remove(index: number) {
    assertTrue(index >= 0 && index < this.data.length);
    this.data.splice(index, 1);
    this.save();
  }

  private load(): QueryHistory {
    const value = window.localStorage.getItem(QUERY_HISTORY_KEY);
    if (value === null) {
      return [];
    }
    const res = QUERY_HISTORY_SCHEMA.safeParse(JSON.parse(value));
    return res.success ? res.data : [];
  }

  private save() {
    window.localStorage.setItem(QUERY_HISTORY_KEY, JSON.stringify(this.data));
  }
}

const QUERY_HISTORY_ENTRY_SCHEMA = z.object({
  query: z.string(),
  starred: z.boolean().default(false),
});

type QueryHistoryEntry = z.infer<typeof QUERY_HISTORY_ENTRY_SCHEMA>;

const QUERY_HISTORY_SCHEMA = z.array(QUERY_HISTORY_ENTRY_SCHEMA);

type QueryHistory = z.infer<typeof QUERY_HISTORY_SCHEMA>;

export const queryHistoryStorage = new HistoryStorage();
