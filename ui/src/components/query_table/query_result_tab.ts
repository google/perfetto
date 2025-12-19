// Copyright (C) 2023 The Android Open Source Project
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
import {v4 as uuidv4} from 'uuid';
import {QueryResponse, runQueryForQueryTable} from './queries';
import {QueryResultsTable} from './query_table';
import {Trace} from '../../public/trace';
import {Tab} from '../../public/tab';

interface QueryResultTabConfig {
  readonly query: string;
  readonly title: string;
  // Optional data to display in this tab instead of fetching it again
  // (e.g. when duplicating an existing tab which already has the data).
  readonly prefetchedResponse?: QueryResponse;
}

// External interface for adding a new query results tab
// Automatically decided whether to add v1 or v2 tab
export function addQueryResultsTab(
  trace: Trace,
  config: QueryResultTabConfig,
  tag?: string,
): void {
  const queryResultsTab = new QueryResultTab(trace, config);

  const uri = 'queryResults#' + (tag ?? uuidv4());

  trace.tabs.registerTab({
    uri,
    content: queryResultsTab,
    isEphemeral: true,
  });
  trace.tabs.showTab(uri);
}

export class QueryResultTab implements Tab {
  private queryResponse?: QueryResponse;

  constructor(
    private readonly trace: Trace,
    private readonly args: QueryResultTabConfig,
  ) {
    this.initTrack();
  }

  private async initTrack() {
    if (this.args.prefetchedResponse !== undefined) {
      this.queryResponse = this.args.prefetchedResponse;
    } else {
      const result = await runQueryForQueryTable(
        this.args.query,
        this.trace.engine,
      );
      this.queryResponse = result;
    }
  }

  getTitle(): string {
    const suffix = this.queryResponse
      ? ` (${this.queryResponse.rows.length})`
      : '';
    return `${this.args.title}${suffix}`;
  }

  render(): m.Children {
    return m(QueryResultsTable, {
      isLoading: this.isLoading(),
      trace: this.trace,
      query: this.args.query,
      resp: this.queryResponse,
      fillHeight: true,
    });
  }

  isLoading() {
    return this.queryResponse === undefined;
  }
}
