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
import {assertExists} from '../../base/logging';
import {QueryResponse, runQueryForQueryTable} from './queries';
import {QueryError, Row, SqlValue} from '../../trace_processor/query_result';
import {AddDebugTrackMenu} from '../tracks/add_debug_track_menu';
import {Button} from '../../widgets/button';
import {PopupMenu} from '../../widgets/menu';
import {PopupPosition} from '../../widgets/popup';
import {QueryTable} from './query_table';
import {Trace} from '../../public/trace';
import {Tab} from '../../public/tab';
import {addLegacyTableTab} from '../details/sql_table_tab';
import {createTableColumn} from '../widgets/sql/table/columns';
import {
  PerfettoSqlType,
  PerfettoSqlTypes,
} from '../../trace_processor/perfetto_sql_type';
import {SqlTableDescription} from '../widgets/sql/table/table_description';

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
  private sqlViewName?: string;

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
      if (result.error !== undefined) {
        return;
      }
    }

    // TODO(stevegolton): Do we really need to create this view upfront?
    this.sqlViewName = await this.createViewForDebugTrack(uuidv4());
  }

  getTitle(): string {
    const suffix = this.queryResponse
      ? ` (${this.queryResponse.rows.length})`
      : '';
    return `${this.args.title}${suffix}`;
  }

  render(): m.Children {
    return m(QueryTable, {
      trace: this.trace,
      query: this.args.query,
      resp: this.queryResponse,
      fillHeight: true,
      contextButtons: [
        this.sqlViewName === undefined
          ? null
          : m(
              PopupMenu,
              {
                trigger: m(Button, {label: 'Show debug track'}),
                position: PopupPosition.Top,
              },
              m(AddDebugTrackMenu, {
                trace: this.trace,
                query: `select * from ${this.sqlViewName}`,
                availableColumns: assertExists(this.queryResponse).columns,
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
      : this.args.query;
    try {
      const createViewResult = await this.trace.engine.query(
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

export function uuidToViewName(uuid: string): string {
  return `view_${uuid.split('-').join('_')}`;
}

// Detected value type for a column, based on the actual values in the result.
type DetectedValueType = 'bigint' | 'number' | 'string' | 'blob' | 'unknown';

// Detects the value type for a column by examining all non-null values.
// Returns 'unknown' if there are no non-null values or if there are mixed
// types.
function detectColumnValueType(
  rows: Row[],
  columnName: string,
): DetectedValueType {
  let detectedType: DetectedValueType | undefined;

  for (const row of rows) {
    const value: SqlValue = row[columnName];

    // Skip null values
    if (value === null) {
      continue;
    }

    let valueType: DetectedValueType;
    if (typeof value === 'bigint') {
      valueType = 'bigint';
    } else if (typeof value === 'number') {
      valueType = 'number';
    } else if (typeof value === 'string') {
      valueType = 'string';
    } else if (value instanceof Uint8Array) {
      valueType = 'blob';
    } else {
      valueType = 'unknown';
    }

    if (detectedType === undefined) {
      detectedType = valueType;
    } else if (detectedType !== valueType) {
      // Mixed types detected
      return 'unknown';
    }
  }

  return detectedType ?? 'unknown';
}

// Infers the PerfettoSQL type for a column based on its name and detected
// value type. For bigint columns:
// - "ts" or columns ending with "_ts" are treated as timestamps
// - "dur" or columns ending with "_dur" are treated as durations
// - "upid" is treated as a process ID reference
// - "utid" is treated as a thread ID reference
// - "arg_set_id" is treated as an arg set ID
function inferColumnType(
  columnName: string,
  valueType: DetectedValueType,
): PerfettoSqlType | undefined {
  if (valueType !== 'bigint') {
    return undefined;
  }

  const lowerName = columnName.toLowerCase();

  // Check for timestamp columns
  if (lowerName === 'ts' || lowerName.endsWith('_ts')) {
    return PerfettoSqlTypes.TIMESTAMP;
  }

  // Check for duration columns
  if (lowerName === 'dur' || lowerName.endsWith('_dur')) {
    return PerfettoSqlTypes.DURATION;
  }

  // Check for process ID columns
  if (lowerName === 'upid') {
    return {kind: 'joinid', source: {table: 'process', column: 'id'}};
  }

  // Check for thread ID columns
  if (lowerName === 'utid') {
    return {kind: 'joinid', source: {table: 'thread', column: 'id'}};
  }

  // Check for arg set ID columns
  if (lowerName === 'arg_set_id') {
    return PerfettoSqlTypes.ARG_SET_ID;
  }

  return undefined;
}

// Creates a SqlTableDescription from query results by detecting column types.
function createTableDescriptionFromQueryResults(
  trace: Trace,
  viewName: string,
  columns: string[],
  rows: Row[],
): SqlTableDescription {
  const tableColumns = columns.map((columnName) => {
    const valueType = detectColumnValueType(rows, columnName);
    const type = inferColumnType(columnName, valueType);
    return createTableColumn({
      trace,
      column: columnName,
      type,
    });
  });

  return {
    name: viewName,
    displayName: 'Query Results',
    columns: tableColumns,
  };
}
