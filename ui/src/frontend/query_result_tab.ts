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

import {QueryResponse, runQuery} from '../common/queries';
import {
  addTab,
  BottomTab,
  bottomTabRegistry,
  closeTab,
  NewBottomTabArgs,
} from './bottom_tab';
import {globals} from './globals';
import {QueryTable} from './query_table';


export function runQueryInNewTab(query: string, title: string, tag?: string) {
  return addTab({
    kind: QueryResultTab.kind,
    tag,
    config: {
      query,
      title,
    },
  });
}

interface QueryResultTabConfig {
  readonly query: string;
  readonly title: string;
  // Optional data to display in this tab instead of fetching it again
  // (e.g. when duplicating an existing tab which already has the data).
  readonly prefetchedResponse?: QueryResponse;
}

export class QueryResultTab extends BottomTab<QueryResultTabConfig> {
  static readonly kind = 'org.perfetto.QueryResultTab';

  queryResponse?: QueryResponse;

  static create(args: NewBottomTabArgs): QueryResultTab {
    return new QueryResultTab(args);
  }

  constructor(args: NewBottomTabArgs) {
    super(args);

    if (this.config.prefetchedResponse !== undefined) {
      this.queryResponse = this.config.prefetchedResponse;
    } else {
      runQuery(this.config.query, this.engine).then((result: QueryResponse) => {
        this.queryResponse = result;
        globals.rafScheduler.scheduleFullRedraw();
      });
    }
  }

  getTitle(): string {
    const suffix =
        this.queryResponse ? ` (${this.queryResponse.rows.length})` : '';
    return `${this.config.title}${suffix}`;
  }

  viewTab(): void {
    return m(QueryTable, {
      query: this.config.query,
      resp: this.queryResponse,
      onClose: () => closeTab(this.uuid),
    });
  }

  renderTabCanvas() {}
}

bottomTabRegistry.register(QueryResultTab);
