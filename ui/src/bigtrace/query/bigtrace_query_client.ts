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

import type {Row as DataGridRow} from '../../trace_processor/query_result';
import type {Filter} from '../../components/widgets/datagrid/model';
import type {SettingFilter} from '../settings/settings_types';
import {coerceFiltersForWire} from './filter_encoding';
import type {RawQueryExecution} from './query_history_storage';

// Tabular wire shape. Values are always strings; JSON null denotes SQL NULL.
interface QueryResponsePayload {
  queryUuid?: string;
  columnNames?: string[];
  rows?: Array<{values: Array<string | null>}>;
  // Filtered count for scrollbar sizing.
  totalFilteredRows?: number;
  // Every column the client could project (result + metadata columns), so the
  // picker can offer ones the current projection omits. `:fetch_results`-only.
  availableColumnNames?: string[];
}

export interface QueryResultPage {
  readonly rows: ReadonlyArray<DataGridRow>;
  readonly columns: ReadonlyArray<string>;
  readonly queryUuid?: string;
  // Post-filter count from `:fetch_results`; undefined elsewhere.
  readonly totalFilteredRows?: number;
  // Every projectable column (result + metadata columns); `:fetch_results`-only.
  readonly availableColumnNames?: ReadonlyArray<string>;
}

// One column the `/trace_metadata` endpoint can return for the current trace
// source. `defaultVisible` flags the columns the grid shows on first render;
// `type` is informational (the wire is always-strings).
export interface TraceColumnDescriptor {
  readonly name: string;
  readonly type: string;
  readonly defaultVisible: boolean;
  readonly description?: string;
}

// `/trace_metadata_schema` response: the column catalog for the trace-list
// grid + the column-picker widget.
export interface TracesSchemaResponse {
  readonly columns: ReadonlyArray<TraceColumnDescriptor>;
}

// The submit-time trace-selection snapshot shipped as top-level fields on
// /execute_*. Each is omitted from the wire when empty / default, so a query
// run with no trace selection sends just the base limit/perfetto_sql/settings.
export interface ExecuteOptions {
  // Structured filter picking which traces the query runs over. Shipped as a
  // native JSON array via coerceFiltersForWire.
  readonly traceFilters?: ReadonlyArray<Filter>;
  // Trace-metadata columns to attach to each result row.
  readonly traceMetadataColumns?: ReadonlyArray<string>;
  // AIP-132 ordering for the trace processing order.
  readonly traceOrderBy?: string;
  // Non-negative cap on traces fanned out; 0 / undefined means no cap.
  readonly traceLimit?: number;
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

// Backend returned a non-OK HTTP status (other than 404, which maps to
// QueryNotFoundError). `detail` is the human-readable `detail` from the error
// body (or the raw body when it isn't JSON); `status` is the HTTP status, so
// callers can render the detail and branch on the status instead of scraping
// the message string.
export class BigtraceHttpError extends Error {
  constructor(
    readonly status: number,
    readonly detail: string,
  ) {
    super(`HTTP ${status}: ${detail}`);
    this.name = 'BigtraceHttpError';
  }
}

// Single funnel for the BigTrace HTTP API.
export class BigtraceQueryClient {
  constructor(private readonly endpoint: string) {}

  // ----- Query execution -----

  async executeSync(
    query: string,
    limit: number,
    settings: ReadonlyArray<SettingFilter>,
    signal?: AbortSignal,
    options?: ExecuteOptions,
  ): Promise<QueryResultPage> {
    return this.executeAt(
      '/execute_bigtrace_query',
      query,
      limit,
      settings,
      signal,
      options,
    );
  }

  async executeAsync(
    query: string,
    limit: number,
    settings: ReadonlyArray<SettingFilter>,
    signal?: AbortSignal,
    options?: ExecuteOptions,
  ): Promise<QueryResultPage> {
    return this.executeAt(
      '/execute_bigtrace_query_async',
      query,
      limit,
      settings,
      signal,
      options,
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

  // Page a query's results. POST body: `limit`/`offset` plus optional
  // `order_by` (AIP-132), `filters` (native Filter[]), and `columns` field-mask
  // over (result cols + metadata columns). The response echoes that set as
  // `availableColumnNames`. Mid-flight calls return whatever rows are ready.
  async fetchResults(
    uuid: string,
    limit: number,
    offset: number,
    signal?: AbortSignal,
    orderBy?: string,
    filter?: ReadonlyArray<Filter>,
    columns?: ReadonlyArray<string>,
  ): Promise<QueryResultPage> {
    const body: Record<string, unknown> = {limit, offset};
    if (orderBy && orderBy.length > 0) {
      body.order_by = orderBy;
    }
    if (filter && filter.length > 0) {
      body.filters = coerceFiltersForWire(filter);
    }
    if (columns && columns.length > 0) {
      body.columns = [...columns];
    }
    const result = await this.requestJson<QueryResponsePayload>(
      `/query_executions/${uuid}:fetch_results`,
      {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(body),
        signal,
      },
    );
    // `availableColumnNames` is `:fetch_results`-only, so the shared parser
    // stays endpoint-agnostic and only this call site exposes the field.
    return {
      ...parseQueryResponse(result),
      availableColumnNames: result.availableColumnNames,
    };
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

  // Paginated trace metadata for the current trace source — the data behind
  // the Settings-page trace-selection grid. `filter` / `order_by` / `columns`
  // mirror `:fetch_results`; `filter` ships as a native JSON array, NOT a
  // JSON-encoded string.
  async listTraceMetadata(
    settings: ReadonlyArray<SettingFilter>,
    limit: number,
    offset: number,
    signal?: AbortSignal,
    orderBy?: string,
    filter?: ReadonlyArray<Filter>,
    columns?: ReadonlyArray<string>,
  ): Promise<QueryResultPage> {
    const body: Record<string, unknown> = {
      settings: this.settingsToWire(settings),
      limit,
      offset,
    };
    if (orderBy && orderBy.length > 0) {
      body.order_by = orderBy;
    }
    if (filter && filter.length > 0) {
      body.filters = coerceFiltersForWire(filter);
    }
    if (columns && columns.length > 0) {
      body.columns = [...columns];
    }
    const result = await this.requestJson<QueryResponsePayload>(
      '/trace_metadata',
      {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(body),
        signal,
      },
    );
    return parseQueryResponse(result);
  }

  // Column catalog for `/trace_metadata`, fetched once on Settings-page load
  // to build the grid's schema + the column-picker. `settings` lets the
  // response vary when the schema depends on the trace source.
  async listTraceMetadataSchema(
    settings: ReadonlyArray<SettingFilter>,
    signal?: AbortSignal,
  ): Promise<TracesSchemaResponse> {
    return this.requestJson<TracesSchemaResponse>('/trace_metadata_schema', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({
        settings: this.settingsToWire(settings),
      }),
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
    options?: ExecuteOptions,
  ): Promise<QueryResultPage> {
    const body: Record<string, unknown> = {
      limit,
      perfetto_sql: query,
      settings: this.settingsToWire(settings),
    };
    // Each trace-selection field rides only when non-default.
    if (options?.traceFilters && options.traceFilters.length > 0) {
      body.trace_filters = coerceFiltersForWire(options.traceFilters);
    }
    if (
      options?.traceMetadataColumns &&
      options.traceMetadataColumns.length > 0
    ) {
      body.trace_metadata_columns = [...options.traceMetadataColumns];
    }
    if (options?.traceOrderBy && options.traceOrderBy.length > 0) {
      body.trace_order_by = options.traceOrderBy;
    }
    if (options?.traceLimit !== undefined && options.traceLimit > 0) {
      body.trace_limit = options.traceLimit;
    }
    const result = await this.requestJson<QueryResponsePayload>(path, {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(body),
      signal,
    });
    return parseQueryResponse(result);
  }

  // camelCase SettingFilter[] -> the snake_case {setting_id, values, category}
  // shape every POST body that ships settings uses (/trace_metadata,
  // /trace_metadata_schema, /execute_*).
  private settingsToWire(settings: ReadonlyArray<SettingFilter>) {
    return settings.map((s) => ({
      setting_id: s.settingId,
      values: s.values,
      category: s.category,
    }));
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
        // Most 403s here are an unauthenticated session; fold the hint into
        // the detail so it reaches the user with the rest of the message.
        throw new BigtraceHttpError(
          403,
          `${detail} (this may be an authentication issue — make sure you ` +
            `are logged in with the correct credentials)`,
        );
      }
      throw new BigtraceHttpError(response.status, detail);
    }
    return response;
  }

  private async requestJson<T>(path: string, init?: RequestInit): Promise<T> {
    const response = await this.request(path, init);
    return (await response.json()) as T;
  }
}

// Passes wire values through as-is: no numeric coercion (would corrupt 64-bit
// ids/timestamps past 2^53), and SQL NULL arrives as JSON null. Do NOT
// special-case the literal string "NULL" — that would corrupt a genuine "NULL"
// string value into SQL NULL.
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
      out[header] = row.values[i];
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
