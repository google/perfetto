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
import type {Engine} from '../../../trace_processor/engine';
import {EmptyState} from '../../../widgets/empty_state';
import type {InstanceRow} from '../types';
import {fmtSize} from '../format';
import {type NavFn, SortableTable, InstanceLink} from '../components';
import * as queries from '../queries';

interface SearchViewAttrs {
  engine: Engine;
  navigate: NavFn;
  initialQuery?: string;
}

function SearchView(): m.Component<SearchViewAttrs> {
  let query = '';
  let results: InstanceRow[] = [];
  let timer: ReturnType<typeof setTimeout> | null = null;
  let searchSeq = 0;

  function doSearch(q: string, engine: Engine) {
    if (q.length < 2) {
      results = [];
      m.redraw();
      return;
    }
    const seq = ++searchSeq;
    queries
      .search(engine, q)
      .then((r) => {
        if (seq !== searchSeq) return;
        results = r;
        m.redraw();
      })
      .catch(console.error);
  }

  function handleChange(q: string, engine: Engine) {
    query = q;
    if (timer) clearTimeout(timer);
    if (q.length < 2) {
      results = [];
      return;
    }
    timer = setTimeout(() => {
      doSearch(q, engine);
    }, 300);
  }

  return {
    oninit(vnode) {
      const {initialQuery, engine} = vnode.attrs;
      query = initialQuery ?? '';
      if (initialQuery !== undefined && initialQuery.length >= 2) {
        doSearch(initialQuery, engine);
      }
    },
    onremove() {
      if (timer) clearTimeout(timer);
    },
    view(vnode) {
      const {navigate, engine} = vnode.attrs;

      return m('div', [
        m('h2', {class: 'ah-view-heading'}, 'Search'),
        m('input', {
          type: 'text',
          value: query,
          oninput: (e: Event) =>
            handleChange((e.target as HTMLInputElement).value, engine),
          placeholder: 'Class name or 0x\u2026 hex id',
          class: 'ah-input',
        }),
        results.length > 0
          ? m(SortableTable, {
              columns: [
                {
                  label: 'Retained',
                  align: 'right',
                  sortKey: (r: InstanceRow) => r.retainedTotal,
                  render: (r: InstanceRow) =>
                    m('span', {class: 'ah-mono'}, fmtSize(r.retainedTotal)),
                },
                {
                  label: 'Object',
                  render: (r: InstanceRow) =>
                    m(InstanceLink, {row: r, navigate}),
                },
              ],
              data: results,
              rowKey: (r: InstanceRow) => r.id,
            })
          : null,
        query.length >= 2 && results.length === 0
          ? m(EmptyState, {icon: 'search', title: 'No results found'})
          : null,
      ]);
    },
  };
}

export default SearchView;
