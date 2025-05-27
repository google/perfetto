// Copyright (C) 2025 The Android Open Source Project
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

import {TextParagraph} from '../../../widgets/text_paragraph';
import {QueryTable} from '../../../components/query_table/query_table';
import {runQueryForQueryTable} from '../../../components/query_table/queries';
import {AsyncLimiter} from '../../../base/async_limiter';
import {QueryResponse} from '../../../components/query_table/queries';
import {Section} from '../../../widgets/section';
import {Trace} from '../../../public/trace';
import {Query, queryToRun} from './query_node_explorer';

export interface NodeDataViewerAttrs {
  readonly query?: Query | Error;
  readonly executeQuery: boolean;
  readonly trace: Trace;
  readonly onQueryExecuted: () => void;
}

export class NodeDataViewer implements m.ClassComponent<NodeDataViewerAttrs> {
  private readonly tableAsyncLimiter = new AsyncLimiter();
  private queryResult?: QueryResponse;

  view({attrs}: m.CVnode<NodeDataViewerAttrs>) {
    const runQuery = () => {
      this.tableAsyncLimiter.schedule(async () => {
        if (
          attrs.query === undefined ||
          attrs.query instanceof Error ||
          !attrs.executeQuery
        )
          return;

        this.queryResult = await runQueryForQueryTable(
          queryToRun(attrs.query),
          attrs.trace.engine,
        );
        attrs.onQueryExecuted();
      });
    };
    console.log('NodeDataViewer view called with query:', attrs.query);
    if (attrs.query === undefined) {
      console.log('NodeDataViewer: No query to run');
      return m(TextParagraph, {text: `No data to display}`});
    }
    if (attrs.query instanceof Error) {
      return m(TextParagraph, {text: `Error: ${attrs.query.message}`});
    }
    if (this.queryResult === undefined) {
      runQuery();
      return m(TextParagraph, {text: `No data to display`});
    }
    if (this.queryResult.error !== undefined) {
      return m(TextParagraph, {text: `Error: ${this.queryResult.error}`});
    }

    runQuery();
    return [
      m(
        Section,
        {title: 'Query data'},
        m(QueryTable, {
          trace: attrs.trace,
          query: queryToRun(attrs.query),
          resp: this.queryResult,
          fillParent: false,
        }),
      ),
    ];
  }
}
