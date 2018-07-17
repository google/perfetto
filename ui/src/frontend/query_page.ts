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

import {RawQueryResult} from '../common/protos';
import {Engine} from '../controller/engine';

import {gEngines} from './globals';
import {createPage} from './pages';

// TODO(hjd): Something mangles unicode in strings – we should fix that.
// A –.
const EMDASH = '\u2014';
const MAX_DISPLAYED_ROWS = 1000;

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

interface QueryResponse {
  query: string;
  success: boolean;
  rowCount: number;
  durationMs: number;
  result: RawQueryResult;
}

const responses: QueryResponse[] = [];

async function doQuery(engine: Engine, query: string) {
  const start = performance.now();
  const log: Partial<QueryResponse> = {
    query,
    success: true,
    rowCount: 0,
  };

  try {
    const result = await engine.rawQuery({
      sqlQuery: query,
    });
    console.log(result);
    log.rowCount = +result.numRecords;
    log.result = result;
  } catch (error) {
    log.success = false;
  }
  const end = performance.now();
  log.durationMs = Math.round(end - start);
  responses.unshift(log as QueryResponse);
  m.redraw();
}

function table(result?: RawQueryResult): m.Children {
  if (!result) return m('');

  const extract =
      (d: RawQueryResult.IColumnValues, i: number): number | string => {
        if (!d || !d.longValues || !d.doubleValues || !d.stringValues) return 0;
        if (d.longValues.length > 0) return +d.longValues[i];
        if (d.doubleValues.length > 0) return +d.doubleValues[i];
        if (d.stringValues.length > 0) return d.stringValues[i];
        return 0;
      };
  const rows = result.numRecords;
  const rowsToDisplay = Math.min(+rows, MAX_DISPLAYED_ROWS);
  return m(
      'table',
      m('thead', m('tr', result.columnDescriptors.map(d => m('th', d.name)))),
      m('tbody', [...Array.from({length: rowsToDisplay}).keys()].map(i => {
        return m('tr', result.columns.map((c: RawQueryResult.IColumnValues) => {
          return m('td', extract(c, i));
        }));
      })));
}

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
  view() {
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
          onsubmit: (e: Event) => {
            e.preventDefault();
            console.log(this.query);
            const engine = gEngines.get('0');
            if (!engine) return;
            doQuery(engine, this.query);
          },
        },
        m('input.query-input', {
          placeholder: 'Query',
          disabled: !gEngines.get('0'),
          oninput: m.withAttr('value', (q: string) => this.query = q),
          value: this.query,
        }),
        examples);
  },
} as m.Component<{}, {query: string}>;

function createQueryResponse(entry: QueryResponse) {
  const stats = [
    entry.rowCount > MAX_DISPLAYED_ROWS ?
        `first ${MAX_DISPLAYED_ROWS} of ${entry.rowCount} rows` :
        `${entry.rowCount} rows`,
    EMDASH,
    `${entry.durationMs} ms`,
  ].join(' ');
  return m(
      '.query-log-entry',
      m('.query-log-entry-query', entry.query),
      m('.query-log-entry-stats', stats),
      m('.query-log-entry-result', table(entry.result)));
}

export const QueryPage = createPage({
  view() {
    return m('.query-page', m(QueryBox), responses.map(createQueryResponse));
  }
});
