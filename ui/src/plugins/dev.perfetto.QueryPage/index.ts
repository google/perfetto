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
import {QueryResultsTable} from '../../components/query_table/query_table';
import {Flag} from '../../public/feature_flag';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {QueryPage} from './query_page';
import {queryHistoryStorage} from '../../components/widgets/query_history';
import {EmptyState} from '../../widgets/empty_state';
import {Anchor} from '../../widgets/anchor';

export default class QueryPagePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.QueryPage';
  static addQueryPageMiniFlag: Flag;

  async onTraceLoad(trace: Trace): Promise<void> {
    // The query page and tab share the same query data.
    let executedQuery: string | undefined;
    let queryResult: QueryResponse | undefined;
    let isLoading = false;
    let editorText = '';

    async function onExecute(text: string) {
      if (!text) return;

      executedQuery = text;
      queryResult = undefined;
      queryHistoryStorage.saveQuery(text);

      isLoading = true;
      queryResult = await runQueryForQueryTable(text, trace.engine);
      isLoading = false;

      trace.tabs.showTab('dev.perfetto.QueryPage');
    }

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

    trace.tabs.registerTab({
      uri: 'dev.perfetto.QueryPage',
      isEphemeral: false,
      content: {
        render() {
          return m(QueryResultsTable, {
            trace,
            isLoading,
            query: executedQuery,
            resp: queryResult,
            fillHeight: true,
            emptyState: m(
              EmptyState,
              {
                fillHeight: true,
                title: 'No query results',
              },
              [
                'Execute a query in the ',
                m(Anchor, {href: '#!/query'}, 'Query Page'),
                ' to see results here.',
              ],
            ),
          });
        },
        getTitle() {
          return 'Query Page Results';
        },
      },
    });
  }
}
