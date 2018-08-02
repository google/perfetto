// Copyright (C) 2018 The Android Open Source Project
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

import {executeQuery} from '../common/actions';
import {Row} from '../common/protos';
import {QueryResponse} from '../common/queries';
import {QueryConfig} from '../common/state';

import {globals} from './globals';
import {quietDispatch} from './mithril_helpers';
import {createPage} from './pages';

// TODO(hjd): Something mangles unicode in strings – we should fix that.
// A –.
const EMDASH = '\u2014';

interface ExampleQuery {
  name: string;
  query: string;
}

const ExampleQueries = [
  {name: 'All sched slices', query: 'select * from sched;'},
  {
    name: 'Cpu time per Cpu',
    query: 'select cpu, sum(dur)/1000/1000 as ms from sched group by cpu;'
  },
];

const ExampleQuery = {
  view(vnode) {
    return m(
        'a[href=#]',
        {
          onclick: (e: Event) => {
            e.preventDefault();
            vnode.attrs.chosen();
          },
        },
        vnode.children);
  }
} as m.Component<{chosen: () => void}>;

const QueryBox = {
  view({attrs}) {
    const examples: m.Children = ['Examples: '];
    for (let i = 0; i < ExampleQueries.length; i++) {
      if (i !== 0) examples.push(', ');
      examples.push(
          m(ExampleQuery,
            {chosen: () => this.query = ExampleQueries[i].query},
            ExampleQueries[i].name));
    }

    return m(
        'form',
        {
          onsubmit: quietDispatch(e => {
            e.preventDefault();
            return executeQuery(attrs.engineId, this.query);
          }),
        },
        m('input.query-input', {
          placeholder: 'Query',
          oninput: m.withAttr('value', (q: string) => this.query = q),
          value: this.query,
        }),
        examples);
  },
} as m.Component<{engineId: string}, {query: string}>;

function getQueryResponses(engineId: string): Array<Partial<QueryResponse>> {
  const responses: Array<Partial<QueryResponse>> = [];
  for (const config of Object.values<QueryConfig>(globals.state.queries)) {
    if (config.engineId !== engineId) continue;

    const response = globals.published.get(config.id);
    if (response) {
      responses.push(response as QueryResponse);
    } else {
      responses.push({
        id: config.id,
        query: config.query,
      });
    }
  }
  responses.sort((a, b) => +b.id! - +a.id!);
  return responses;
}

function renderTable(columns?: string[], rows?: Row[]): m.Children {
  if (!columns || !rows) return m('');

  return m(
      'table',
      m('thead', m('tr', columns.map(column => m('th', column)))),
      m('tbody',
        rows.map(row => m('tr', columns.map(column => m('td', row[column]))))));
}

function renderQueryResponse(entry: Partial<QueryResponse>): m.Children {
  const stats = [];
  if (entry.durationMs) {
    stats.push(`${entry.durationMs} ms`);
  }
  if (entry.totalRowCount && entry.rows) {
    if (entry.totalRowCount === entry.rows.length) {
      stats.push(`${entry.totalRowCount} rows`);
    } else {
      stats.push(`first ${entry.rows.length} of ${entry.totalRowCount} rows`);
    }
  }

  return m(
      '.query-log-entry',
      m('.query-log-entry-query', entry.query),
      m('.query-log-entry-stats', stats.join(` ${EMDASH} `)),
      m('.query-log-entry-result', renderTable(entry.columns, entry.rows)));
}

export const QueryPage = createPage({
  view() {
    const engineId = m.route.param('engineId');
    return m(
        '#page.query-page',
        m(QueryBox, {engineId}),
        getQueryResponses(engineId).map(renderQueryResponse));
  }
});
