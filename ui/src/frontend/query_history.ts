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

import * as m from 'mithril';

import {Actions} from '../common/actions';

import {globals} from './globals';
import {
  arrayOf,
  bool,
  record,
  runValidator,
  str,
  ValidatedType,
} from '../controller/validators';
import {assertTrue} from '../base/logging';

const QUERY_HISTORY_KEY = 'queryHistory';

export class QueryHistoryComponent implements m.ClassComponent {
  view(): m.Child {
    const unstarred: HistoryItemComponentAttrs[] = [];
    const starred: HistoryItemComponentAttrs[] = [];
    for (let i = queryHistoryStorage.data.length - 1; i >= 0; i--) {
      const entry = queryHistoryStorage.data[i];
      const arr = entry.starred ? starred : unstarred;
      arr.push({index: i, entry});
    }
    return m(
        '.query-history',
        m('header.overview',
          `Query history (${queryHistoryStorage.data.length} queries)`),
        starred.map((attrs) => m(HistoryItemComponent, attrs)),
        unstarred.map((attrs) => m(HistoryItemComponent, attrs)));
  }
}

export interface HistoryItemComponentAttrs {
  index: number;
  entry: QueryHistoryEntry;
}

export class HistoryItemComponent implements
    m.ClassComponent<HistoryItemComponentAttrs> {
  view(vnode: m.Vnode<HistoryItemComponentAttrs>): m.Child {
    const query = vnode.attrs.entry.query;
    return m(
        '.history-item',
        m('.history-item-buttons',
          m('button',
            {
              onclick: () => {
                queryHistoryStorage.setStarred(
                    vnode.attrs.index, !vnode.attrs.entry.starred);
                globals.rafScheduler.scheduleFullRedraw();
              },
            },
            m('i.material-icons',
              vnode.attrs.entry.starred ? 'star' : 'star_outline')),
          m('button',
            {
              onclick: () => {
                globals.dispatch(Actions.executeQuery(
                    {queryId: 'analyze-page-query', query}));
              },
            },
            m('i.material-icons', 'play_arrow')),
          m('button',
            {
              onclick: () => {
                queryHistoryStorage.remove(vnode.attrs.index);
                globals.rafScheduler.scheduleFullRedraw();
              },
            },
            m('i.material-icons', 'delete'))),
        m('pre', query));
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

    return runValidator(queryHistoryValidator, JSON.parse(value)).result;
  }

  private save() {
    window.localStorage.setItem(QUERY_HISTORY_KEY, JSON.stringify(this.data));
  }
}

const queryHistoryEntryValidator = record({query: str(), starred: bool()});

type QueryHistoryEntry = ValidatedType<typeof queryHistoryEntryValidator>;

const queryHistoryValidator = arrayOf(queryHistoryEntryValidator);

type QueryHistory = ValidatedType<typeof queryHistoryValidator>;

export const queryHistoryStorage = new HistoryStorage();
