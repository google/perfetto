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
import {SimpleResizeObserver} from '../base/resize_observer';
import {undoCommonChatAppReplacements} from '../base/string_utils';
import {QueryResponse, runQuery} from '../common/queries';
import {raf} from '../core/raf_scheduler';
import {Callout} from '../widgets/callout';
import {Editor} from '../widgets/editor';
import {PageWithTraceAttrs} from './pages';
import {QueryHistoryComponent, queryHistoryStorage} from './query_history';
import {addQueryResultsTab} from './query_result_tab';
import {QueryTable} from './query_table';
import {Engine, EngineAttrs} from '../trace_processor/engine';
import {assertExists} from '../base/logging';

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

function runManualQuery(engine: Engine, query: string) {
  state.executedQuery = query;
  state.queryResult = undefined;
  runQuery(undoCommonChatAppReplacements(query), engine).then(
    (resp: QueryResponse) => {
      addQueryResultsTab(
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
      raf.scheduleFullRedraw();
    },
  );
  raf.scheduleDelayedFullRedraw();
}

class QueryInput implements m.ClassComponent<EngineAttrs> {
  private resize?: Disposable;

  oncreate({dom}: m.CVnodeDOM<EngineAttrs>): void {
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

  view({attrs}: m.CVnode<EngineAttrs>) {
    return m(Editor, {
      generation: state.generation,
      initialText: state.enteredText,

      onExecute: (text: string) => {
        if (!text) {
          return;
        }
        queryHistoryStorage.saveQuery(text);
        runManualQuery(attrs.engine, text);
      },

      onUpdate: (text: string) => {
        state.enteredText = text;
        raf.scheduleFullRedraw();
      },
    });
  }
}

export class QueryPage implements m.ClassComponent<PageWithTraceAttrs> {
  private engine?: Engine;

  oninit({attrs}: m.CVnode<PageWithTraceAttrs>) {
    this.engine = attrs.trace.engine.getProxy('QueryPage');
  }

  view() {
    const engine = assertExists(this.engine);
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
      m(QueryInput, {engine}),
      state.executedQuery === undefined
        ? null
        : m(QueryTable, {
            query: state.executedQuery,
            resp: state.queryResult,
            fillParent: false,
          }),
      m(QueryHistoryComponent, {
        runQuery: (q: string) => runManualQuery(engine, q),
        setQuery: (q: string) => {
          state.enteredText = q;
          state.generation++;
          raf.scheduleFullRedraw();
        },
      }),
    );
  }
}
