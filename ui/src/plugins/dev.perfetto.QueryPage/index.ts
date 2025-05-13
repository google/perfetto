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

export default class QueryPagePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.QueryPage';
  static flag: Flag;

  static onActivate(app: App) {
    QueryPagePlugin.flag = app.featureFlags.register({
      id: 'dev.perfetto.QueryPage',
      name: 'Enable mini query page tab',
      defaultValue: false,
      description:
        'Enables a tab version of the query page that allows query tab - like functionality in the tab drawer',
    });
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    trace.pages.registerPage({
      route: '/query',
      render: () => m(QueryPage, {trace}),
    });
    trace.sidebar.addMenuItem({
      section: 'current_trace',
      text: 'Query (SQL)',
      href: '#!/query',
      icon: 'database',
      sortOrder: 1,
    });

    if (QueryPagePlugin.flag.get()) {
      trace.tabs.registerTab({
        uri: 'dev.perfetto.QueryPage',
        isEphemeral: false,
        content: {
          render() {
            return m(QueryPageMini, {trace});
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
  readonly trace: Trace;
}

class QueryPageMini implements m.ClassComponent<QueryPageMiniAttrs> {
  private executedQuery?: string;
  private queryResult?: QueryResponse;

  view({attrs}: m.CVnode<QueryPageMiniAttrs>) {
    return m(
      '.pf-query-page-mini',
      m(Editor, {
        language: 'perfetto-sql',
        onExecute: async (query) => {
          this.executedQuery = query;
          const result = await runQueryForQueryTable(query, attrs.trace.engine);
          this.queryResult = result;
        },
      }),
      this.executedQuery === undefined
        ? null
        : m(QueryTable, {
            trace: attrs.trace,
            query: this.executedQuery,
            resp: this.queryResult,
            fillParent: false,
          }),
    );
  }
}
