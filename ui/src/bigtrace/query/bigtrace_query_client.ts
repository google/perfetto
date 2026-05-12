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

// Wire-shape of a tabular response. The backend serializes each row as a
// `values` array aligned with `columnNames`. Cell values are whatever the
// backend can express in JSON; we don't translate them here — the renderer
// receives them as-is and decides how to display.
//
// `queryUuid` is the server-assigned identifier for the run. Modern
// backends emit it as a top-level field on both sync and async submit
// responses. Older / mock backends still ship the async UUID inside a
// `{columnNames: ['queryUuid'], rows: [{values: [uuid]}]}` envelope —
// `parseQueryResponse` falls back to that legacy shape when the
// top-level field is absent.
interface QueryResponsePayload {
  queryUuid?: string;
  columnNames?: string[];
  // Modern backends ship every value as a JSON string (always-
  // strings response contract; see ~/Projects/CLAUDE.md "Response
  // value contract"). The `number` variant stays here because
  // (a) older / mock backends still send typed JSON, and
  // (b) a follow-up will add per-column schema info in the
  // response that lets the UI coerce strings back to typed JS
  // values — that's where the always-strings shape will lock in
  // on the UI side too. Until then, `parseQueryResponse` passes
  // values through whichever shape the backend sends.
  rows?: Array<{values: Array<string | number | null>}>;
  // Modern :fetch_results responses carry the filtered row count so
  // the DataGrid can size its virtual scrollbar over the filtered
  // set. Older / mock backends omit it; clients fall back to the
  // unfiltered total in that case.
  totalFilteredRows?: number;
}

export interface QueryResultPage {
  readonly rows: ReadonlyArray<DataGridRow>;
  readonly columns: ReadonlyArray<string>;
  readonly queryUuid?: string;
  // Present on `:fetch_results` responses from modern backends. The
  // post-filter row count when a filter is set; the materialized
  // table size otherwise. Undefined for `executeSync`/`executeAsync`
  // responses (which have no notion of a filter).
  readonly totalFilteredRows?: number;
}

// Thrown when a request is aborted via its AbortSignal. Callers should treat
// this as a user-initiated cancellation, not an error worth surfacing.
export class QueryCancelledError extends Error {
  constructor() {
    super('Query was cancelled.');
    this.name = 'QueryCancelledError';
  }
}

// Thrown when the backend reports 404 for a query UUID — typically because
// the entry was deleted from history or the backend was restarted while the
// UI still held the stale UUID in localStorage. Distinguished from generic
// HTTP errors so callers can drop the dead reference instead of polling
// forever.
export class QueryNotFoundError extends Error {
  constructor(uuid: string) {
    super(`Query ${uuid} not found on the backend.`);
    this.name = 'QueryNotFoundError';
  }
}

// Single client for the BigTrace HTTP API. All endpoints listed in
// `~/Projects/CLAUDE.md` (BigTrace Backend API section) flow through here so
// auth, error handling, and response shape lives in exactly one place.
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

  /**
   * Page through a finished (or in-flight) async query's materialized
   * result table.
   *
   * - `limit` / `offset` apply **after** ordering and filtering, so
   *   flipping direction starts the user at the top of the new
   *   ordering, and filtering shrinks the universe paginated over.
   * - `orderBy` follows
   *   [AIP-132 §Ordering](https://google.aip.dev/132#ordering):
   *   comma-separated list of `"<field> [asc|desc]"` entries; default
   *   `asc`. Field names must match the materialized table's
   *   columns; the backend rejects unknown fields and malformed
   *   directions with HTTP 400.
   * - `filter` is the DataGrid widget's `Filter[]` shape. We
   *   JSON-serialize it via `encodeFilters` (always-strings wire,
   *   canonical key order) and ship it as the `filter` query param.
   *   Multi-entry arrays are AND'd by the backend. Same 400 error
   *   surface as `orderBy` for malformed input or unknown columns.
   * - Mid-flight queries (status=IN_PROGRESS) return whatever rows
   *   workers have merged so far. Empty body is normal during the
   *   merge gap.
   */
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
      // fetch() rejects with AbortError when the signal fires.
      if (e instanceof DOMException && e.name === 'AbortError') {
        throw new QueryCancelledError();
      }
      throw e;
    }
    if (!response.ok) {
      const errorText = await response
        .text()
        .catch(() => 'Failed to read response body');
      // BigTrace's wire contract carries human-readable error
      // descriptions in a top-level `detail` field on the JSON body.
      // Extract that so the UI surfaces the reason (e.g. "Query X
      // is not materialized") rather than the raw JSON envelope.
      // Falls back to the body text if it isn't JSON.
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
        // Best-effort UUID extraction from /query_executions/{uuid} or
        // /query_executions/{uuid}:status etc. Falls back to the path if
        // the route doesn't match.
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

// Parse a tabular response into typed rows, preserving the wire types. We do
// NOT coerce numeric-looking strings into numbers: the backend may return
// 64-bit integers (timestamps, ids) as strings to avoid JS Number precision
// loss, and silently widening them would corrupt the data.
//
// The single translation we do is the SQL-NULL marker: the backend uses the
// string 'NULL' to represent SQL NULL in JSON, which we surface as a real
// JS null so renderers can branch on `value === null`.
//
// Exported for unit tests.
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
  // Prefer the top-level `queryUuid` (modern wire shape). Legacy
  // shape stuffed the uuid into a single-cell tabular response with
  // a `queryUuid` column — recover it from `rows[0]` if the
  // top-level field is absent.
  let queryUuid = result.queryUuid;
  if (queryUuid === undefined && rows.length === 1) {
    const v = rows[0]['queryUuid'];
    if (typeof v === 'string') queryUuid = v;
  }
  return {
    rows,
    columns,
    queryUuid,
    totalFilteredRows: result.totalFilteredRows,
  };
}
