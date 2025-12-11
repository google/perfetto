// Copyright (C) 2024 The Android Open Source Project
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
import {
  QueryResponse,
  runQueryForQueryTable,
} from '../../components/query_table/queries';
import {QueryTable} from '../../components/query_table/query_table';
import {App} from '../../public/app';
import {Flag} from '../../public/feature_flag';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {Editor} from '../../widgets/editor';
import {QueryPage} from './query_page';
import {queryHistoryStorage} from '../../components/widgets/query_history';
import {ResizeHandle} from '../../widgets/resize_handle';
import {findRef, toHTMLElement} from '../../base/dom_utils';
import {assertExists} from '../../base/logging';
import {addQueryResultsTab} from '../../components/query_table/query_result_tab';

export default class QueryPagePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.QueryPage';
  static addQueryPageMiniFlag: Flag;

  static onActivate(app: App) {
    QueryPagePlugin.addQueryPageMiniFlag = app.featureFlags.register({
      id: 'dev.perfetto.QueryPage',
      name: 'Enable mini query page tab',
      defaultValue: false,
      description:
        'Enables a tab version of the query page that allows query tab - like functionality in the tab drawer',
    });
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    // The query page and tab share the same query data.
    let executedQuery: string | undefined;
    let queryResult: QueryResponse | undefined;
    let editorText = '';

    const onExecute = async (text: string) => {
      if (!text) {
        return;
      }

      queryHistoryStorage.saveQuery(text);

      executedQuery = text;
      queryResult = undefined;
      queryResult = await runQueryForQueryTable(text, trace.engine);

      // TODO(stevegolton): Just show the mini query page instead of adding an
      // ephemeral tab.

      // if (QueryPagePlugin.addQueryPageMiniFlag.get()) {
      //   trace.tabs.showTab('dev.perfetto.QueryPage');
      // }

      addQueryResultsTab(
        trace,
        {
          query: executedQuery,
          title: 'Standalone Query',
          prefetchedResponse: queryResult,
        },
        'analyze_page_query',
      );
    };

    trace.pages.registerPage({
      route: '/query',
      render: () =>
        m(QueryPage, {
          trace,
          editorText,
          executedQuery,
          queryResult,
          onEditorContentUpdate: (text) => (editorText = text),
          onExecute,
        }),
    });

    trace.sidebar.addMenuItem({
      section: 'current_trace',
      text: 'Query (SQL)',
      href: '#!/query',
      icon: 'database',
      sortOrder: 20,
    });

    if (QueryPagePlugin.addQueryPageMiniFlag.get()) {
      trace.tabs.registerTab({
        uri: 'dev.perfetto.QueryPage',
        isEphemeral: false,
        content: {
          render() {
            return m(QueryPageMini, {
              trace,
              editorText,
              executedQuery,
              queryResult,
              onEditorContentUpdate: (text) => (editorText = text),
              onExecute,
            });
          },
          getTitle() {
            return 'QueryPage Mini';
          },
        },
      });
    }
  }
}

interface QueryPageMiniAttrs {
  trace: Trace;
  editorText: string;
  executedQuery?: string;
  queryResult?: QueryResponse;
  onEditorContentUpdate?(content: string): void;
  onExecute?(query: string): void;
}

class QueryPageMini implements m.ClassComponent<QueryPageMiniAttrs> {
  private editorHeight: number = 0;
  private editorElement?: HTMLElement;

  oncreate({dom}: m.VnodeDOM<QueryPageMiniAttrs>) {
    this.editorElement = toHTMLElement(assertExists(findRef(dom, 'editor')));
    this.editorElement.style.height = '200px';
  }

  view({attrs}: m.Vnode<QueryPageMiniAttrs>): m.Children {
    return m(
      '.pf-query-page-mini',

      m(Editor, {
        ref: 'editor',
        language: 'perfetto-sql',
        onUpdate: attrs.onEditorContentUpdate,
        onExecute: attrs.onExecute,
      }),
      m(ResizeHandle, {
        onResize: (deltaPx: number) => {
          this.editorHeight += deltaPx;
          this.editorElement!.style.height = `${this.editorHeight}px`;
        },
        onResizeStart: () => {
          this.editorHeight = this.editorElement!.clientHeight;
        },
      }),
      attrs.executedQuery === undefined
        ? null
        : m(QueryTable, {
            trace: attrs.trace,
            query: attrs.executedQuery,
            resp: attrs.queryResult,
            fillHeight: false,
          }),
    );
  }
}
