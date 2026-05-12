// Copyright (C) 2026 The Android Open Source Project
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

import {Row as DataGridRow} from '../../trace_processor/query_result';
import {Filter} from '../../components/widgets/datagrid/model';
import {SettingFilter} from '../settings/settings_types';
import {encodeFilters} from './filter_encoding';
import {RawQueryExecution} from './query_history_storage';

// Tabular wire shape. Values are always strings (see CLAUDE.md
// "Response value contract"); `null` denotes SQL NULL.
interface QueryResponsePayload {
  queryUuid?: string;
  columnNames?: string[];
  rows?: Array<{values: Array<string | null>}>;
  // Filtered count for scrollbar sizing.
  totalFilteredRows?: number;
}

export interface QueryResultPage {
  readonly rows: ReadonlyArray<DataGridRow>;
  readonly columns: ReadonlyArray<string>;
  readonly queryUuid?: string;
  // Post-filter count from `:fetch_results`; undefined elsewhere.
  readonly totalFilteredRows?: number;
}

// Request aborted via AbortSignal — treat as cancellation, not an error.
export class QueryCancelledError extends Error {
  constructor() {
    super('Query was cancelled.');
    this.name = 'QueryCancelledError';
  }
}

// Backend returned 404 for a UUID; distinct from generic HTTP errors so
// callers can drop the dead reference instead of polling forever.
export class QueryNotFoundError extends Error {
  constructor(uuid: string) {
    super(`Query ${uuid} not found on the backend.`);
    this.name = 'QueryNotFoundError';
  }
}

// Single funnel for the BigTrace HTTP API (see CLAUDE.md for endpoints).
export class BigtraceQueryClient {
  constructor(private readonly endpoint: string) {}

  // ----- Query execution -----

  async executeSync(
    query: string,
    limit: number,
    settings: ReadonlyArray<SettingFilter>,
    signal?: AbortSignal,
  ): Promise<QueryResultPage> {
    return this.executeAt(
      '/execute_bigtrace_query',
      query,
      limit,
      settings,
      signal,
    );
  }

  async executeAsync(
    query: string,
    limit: number,
    settings: ReadonlyArray<SettingFilter>,
    signal?: AbortSignal,
  ): Promise<QueryResultPage> {
    return this.executeAt(
      '/execute_bigtrace_query_async',
      query,
      limit,
      settings,
      signal,
    );
  }

  async getStatus(
    uuid: string,
    signal?: AbortSignal,
  ): Promise<RawQueryExecution> {
    return this.requestJson<RawQueryExecution>(
      `/query_executions/${uuid}:status`,
      {signal},
    );
  }

  async getQueryExecution(
    uuid: string,
    signal?: AbortSignal,
  ): Promise<RawQueryExecution> {
    return this.requestJson<RawQueryExecution>(`/query_executions/${uuid}`, {
      signal,
    });
  }

  // Page the materialized table; `limit`/`offset` apply after orderBy/filter.
  // `orderBy` is AIP-132; `filter` is the DataGrid `Filter[]` shape encoded
  // via `encodeFilters`. Mid-flight calls return whatever rows have merged.
  async fetchResults(
    uuid: string,
    limit: number,
    offset: number,
    signal?: AbortSignal,
    orderBy?: string,
    filter?: ReadonlyArray<Filter>,
  ): Promise<QueryResultPage> {
    let path = `/query_executions/${uuid}:fetch_results?limit=${limit}&offset=${offset}`;
    if (orderBy && orderBy.length > 0) {
      path += `&order_by=${encodeURIComponent(orderBy)}`;
    }
    if (filter && filter.length > 0) {
      path += `&filter=${encodeURIComponent(encodeFilters(filter))}`;
    }
    const result = await this.requestJson<QueryResponsePayload>(path, {signal});
    return parseQueryResponse(result);
  }

  async cancelQuery(uuid: string, signal?: AbortSignal): Promise<void> {
    await this.request(`/query_executions/${uuid}:cancel`, {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({}),
      signal,
    });
  }

  async listQueryExecutions(
    signal?: AbortSignal,
  ): Promise<ReadonlyArray<RawQueryExecution>> {
    const result = await this.requestJson<{
      queryExecutions?: RawQueryExecution[];
    }>('/query_executions', {signal});
    return result.queryExecutions ?? [];
  }

  async deleteQueryExecution(
    uuid: string,
    signal?: AbortSignal,
  ): Promise<void> {
    await this.request(`/query_executions/${uuid}`, {
      method: 'DELETE',
      signal,
    });
  }

  // ----- Internals -----

  private async executeAt(
    path: string,
    query: string,
    limit: number,
    settings: ReadonlyArray<SettingFilter>,
    signal: AbortSignal | undefined,
  ): Promise<QueryResultPage> {
    const body = JSON.stringify({
      limit,
      perfetto_sql: query,
      settings: settings.map((s) => ({
        setting_id: s.settingId,
        values: s.values,
        category: s.category,
      })),
    });
    const result = await this.requestJson<QueryResponsePayload>(path, {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body,
      signal,
    });
    return parseQueryResponse(result);
  }

  private async request(path: string, init?: RequestInit): Promise<Response> {
    let response: Response;
    try {
      response = await fetch(`${this.endpoint}${path}`, {
        credentials: 'include',
        mode: 'cors',
        ...init,
      });
    } catch (e) {
      // AbortSignal → DOMException, surface as our typed error.
      if (e instanceof DOMException && e.name === 'AbortError') {
        throw new QueryCancelledError();
      }
      throw e;
    }
    if (!response.ok) {
      const errorText = await response
        .text()
        .catch(() => 'Failed to read response body');
      // Surface backend `detail` field; fall back to raw body.
      let detail = errorText;
      try {
        const parsed = JSON.parse(errorText);
        if (typeof parsed?.detail === 'string') {
          detail = parsed.detail;
        }
      } catch {
        // Not JSON — use the body as-is.
      }
      if (response.status === 404) {
        // Extract UUID from /query_executions/{uuid}[:action]; else use path.
        const m = path.match(/\/query_executions\/([^/:?#]+)/);
        throw new QueryNotFoundError(m ? m[1] : path);
      }
      if (response.status === 403) {
        throw new Error(
          `HTTP error! status: ${response.status}, message: ${detail}. ` +
            `This might be an authentication issue. Please ensure you ` +
            `are logged in with the correct credentials.`,
        );
      }
      throw new Error(
        `HTTP error! status: ${response.status}, message: ${detail}`,
      );
    }
    return response;
  }

  private async requestJson<T>(path: string, init?: RequestInit): Promise<T> {
    const response = await this.request(path, init);
    return (await response.json()) as T;
  }
}

// Preserves wire strings as-is (no numeric coercion — would corrupt 64-bit
// ids/timestamps). Only translates 'NULL' to JS null. Exported for unit tests.
export function parseQueryResponse(
  result: QueryResponsePayload,
): QueryResultPage {
  const colNames = result.columnNames;
  if (
    colNames === undefined ||
    colNames === null ||
    result.rows === undefined ||
    result.rows === null
  ) {
    return {rows: [], columns: [], queryUuid: result.queryUuid};
  }

  const columns = colNames.filter((h): h is string => h !== null);
  const rows = result.rows.map((row) => {
    const out: DataGridRow = {};
    for (let i = 0; i < colNames.length; i++) {
      const header = colNames[i];
      if (header === null) continue;
      const value = row.values[i];
      out[header] = value === 'NULL' ? null : value;
    }
    return out;
  });
  return {
    rows,
    columns,
    queryUuid: result.queryUuid,
    totalFilteredRows: result.totalFilteredRows,
  };
}
