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
import {assertExists} from '../../../base/logging';
import {QueryResponse, runQuery} from './queries';
import {QueryError} from '../../../trace_processor/query_result';
import {
  AddDebugTrackMenu,
  uuidToViewName,
} from '../debug_tracks/add_debug_track_menu';
import {Button} from '../../../widgets/button';
import {PopupMenu2} from '../../../widgets/menu';
import {PopupPosition} from '../../../widgets/popup';
import {BottomTab, NewBottomTabArgs} from '../bottom_tab';
import {QueryTable} from './query_table';
import {BottomTabToTabAdapter} from '../../../public/utils';
import {Trace} from '../../../public/trace';

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
  const queryResultsTab = new QueryResultTab({
    trace,
    config,
    uuid: uuidv4(),
  });

  const uri = 'queryResults#' + (tag ?? uuidv4());

  trace.tabs.registerTab({
    uri,
    content: new BottomTabToTabAdapter(queryResultsTab),
    isEphemeral: true,
  });
  trace.tabs.showTab(uri);
}

export class QueryResultTab extends BottomTab<QueryResultTabConfig> {
  static readonly kind = 'dev.perfetto.QueryResultTab';

  queryResponse?: QueryResponse;
  sqlViewName?: string;

  static create(args: NewBottomTabArgs<QueryResultTabConfig>): QueryResultTab {
    return new QueryResultTab(args);
  }

  constructor(args: NewBottomTabArgs<QueryResultTabConfig>) {
    super(args);

    this.initTrack(args);
  }

  async initTrack(args: NewBottomTabArgs<QueryResultTabConfig>) {
    let uuid = '';
    if (this.config.prefetchedResponse !== undefined) {
      this.queryResponse = this.config.prefetchedResponse;
      uuid = args.uuid;
    } else {
      const result = await runQuery(this.config.query, this.engine);
      this.queryResponse = result;
      if (result.error !== undefined) {
        return;
      }

      uuid = uuidv4();
    }

    if (uuid !== '') {
      this.sqlViewName = await this.createViewForDebugTrack(uuid);
      if (this.sqlViewName) {
        this.trace.scheduleRedraw();
      }
    }
  }

  getTitle(): string {
    const suffix = this.queryResponse
      ? ` (${this.queryResponse.rows.length})`
      : '';
    return `${this.config.title}${suffix}`;
  }

  viewTab(): m.Child {
    return m(QueryTable, {
      query: this.config.query,
      resp: this.queryResponse,
      fillParent: true,
      contextButtons: [
        this.sqlViewName === undefined
          ? null
          : m(
              PopupMenu2,
              {
                trigger: m(Button, {label: 'Show debug track'}),
                popupPosition: PopupPosition.Top,
              },
              m(AddDebugTrackMenu, {
                trace: this.trace,
                dataSource: {
                  sqlSource: `select * from ${this.sqlViewName}`,
                  columns: assertExists(this.queryResponse).columns,
                },
              }),
            ),
      ],
    });
  }

  isLoading() {
    return this.queryResponse === undefined;
  }

  async createViewForDebugTrack(uuid: string): Promise<string> {
    const viewId = uuidToViewName(uuid);
    // Assuming that the query results come from a SELECT query, try creating a
    // view to allow us to reuse it for further queries.
    const hasValidQueryResponse =
      this.queryResponse && this.queryResponse.error === undefined;
    const sqlQuery = hasValidQueryResponse
      ? this.queryResponse!.lastStatementSql
      : this.config.query;
    try {
      const createViewResult = await this.engine.query(
        `create view ${viewId} as ${sqlQuery}`,
      );
      if (createViewResult.error()) {
        // If it failed, do nothing.
        return '';
      }
    } catch (e) {
      if (e instanceof QueryError) {
        // If it failed, do nothing.
        return '';
      }
      throw e;
    }
    return viewId;
  }
}
