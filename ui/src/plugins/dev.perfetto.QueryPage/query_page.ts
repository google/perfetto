// Copyright (C) 2020 The Android Open Source Project
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
import {SimpleResizeObserver} from '../../base/resize_observer';
import {undoCommonChatAppReplacements} from '../../base/string_utils';
import {QueryResponse, runQuery} from '../../public/lib/query_table/queries';
import {Callout} from '../../widgets/callout';
import {Editor} from '../../widgets/editor';
import {PageWithTraceAttrs} from '../../public/page';
import {QueryHistoryComponent, queryHistoryStorage} from './query_history';
import {Trace, TraceAttrs} from '../../public/trace';
import {addQueryResultsTab} from '../../public/lib/query_table/query_result_tab';
import {QueryTable} from '../../public/lib/query_table/query_table';

interface QueryPageState {
  enteredText: string;
  executedQuery?: string;
  queryResult?: QueryResponse;
  heightPx: string;
  generation: number;
}

const state: QueryPageState = {
  enteredText: '',
  heightPx: '100px',
  generation: 0,
};

function runManualQuery(trace: Trace, query: string) {
  state.executedQuery = query;
  state.queryResult = undefined;
  runQuery(undoCommonChatAppReplacements(query), trace.engine).then(
    (resp: QueryResponse) => {
      addQueryResultsTab(
        trace,
        {
          query: query,
          title: 'Standalone Query',
          prefetchedResponse: resp,
        },
        'analyze_page_query',
      );
      // We might have started to execute another query. Ignore it in that
      // case.
      if (state.executedQuery !== query) {
        return;
      }
      state.queryResult = resp;
      trace.scheduleFullRedraw();
    },
  );
}

export type QueryInputAttrs = TraceAttrs;

class QueryInput implements m.ClassComponent<QueryInputAttrs> {
  private resize?: Disposable;

  oncreate({dom}: m.CVnodeDOM<QueryInputAttrs>): void {
    this.resize = new SimpleResizeObserver(dom, () => {
      state.heightPx = (dom as HTMLElement).style.height;
    });
    (dom as HTMLElement).style.height = state.heightPx;
  }

  onremove(): void {
    if (this.resize) {
      this.resize[Symbol.dispose]();
      this.resize = undefined;
    }
  }

  view({attrs}: m.CVnode<QueryInputAttrs>) {
    return m(Editor, {
      generation: state.generation,
      initialText: state.enteredText,

      onExecute: (text: string) => {
        if (!text) {
          return;
        }
        queryHistoryStorage.saveQuery(text);
        runManualQuery(attrs.trace, text);
      },

      onUpdate: (text: string) => {
        state.enteredText = text;
        attrs.trace.scheduleFullRedraw();
      },
    });
  }
}

export class QueryPage implements m.ClassComponent<PageWithTraceAttrs> {
  view({attrs}: m.CVnode<PageWithTraceAttrs>) {
    return m(
      '.query-page',
      m(Callout, 'Enter query and press Cmd/Ctrl + Enter'),
      state.enteredText.includes('"') &&
        m(
          Callout,
          {icon: 'warning'},
          `" (double quote) character observed in query; if this is being used to ` +
            `define a string, please use ' (single quote) instead. Using double quotes ` +
            `can cause subtle problems which are very hard to debug.`,
        ),
      m(QueryInput, attrs),
      state.executedQuery === undefined
        ? null
        : m(QueryTable, {
            trace: attrs.trace,
            query: state.executedQuery,
            resp: state.queryResult,
            fillParent: false,
          }),
      m(QueryHistoryComponent, {
        trace: attrs.trace,
        runQuery: (q: string) => runManualQuery(attrs.trace, q),
        setQuery: (q: string) => {
          state.enteredText = q;
          state.generation++;
          attrs.trace.scheduleFullRedraw();
        },
      }),
    );
  }
}
