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

import {endpointStorage} from '../settings/endpoint_storage';
import {BigtraceQueryClient} from './bigtrace_query_client';
import {QueryExecution} from './query_store';

// Wire shape returned by the BigTrace backend for /query_executions and
// /query_executions/{uuid}[:status]. Field set is documented in
// `~/Projects/CLAUDE.md` (BigTrace Backend API section).
//
// Times are ISO-8601 strings; numeric counters are JS numbers; `limit` is a
// number. The structure is `Readonly` to make wire-shape leaks into mutable
// UI state explicit at the boundary.
export interface RawQueryExecution {
  readonly queryUuid?: string;
  readonly status?: string;
  readonly startTime?: string;
  readonly endTime?: string;
  readonly processedRows?: number;
  readonly processedTraces?: number;
  readonly totalTraces?: number;
  readonly error?: string;
  readonly errorMessage?: string;
  readonly perfettoSql?: string;
  readonly limit?: number;
  readonly materialized?: boolean;
  readonly tableName?: string;
  readonly tableLink?: string;
}

// Convert an ISO-8601 string into epoch milliseconds, or undefined if the
// input is missing or unparseable. Centralized so every layer gets the same
// "invalid date → undefined" semantic instead of silently producing NaN.
export function isoToEpochMs(iso: string | undefined): number | undefined {
  if (iso === undefined) return undefined;
  const ms = new Date(iso).getTime();
  return Number.isFinite(ms) ? ms : undefined;
}

export class QueryHistoryStorage {
  // Build a fresh client for each call so endpoint changes (via Settings)
  // take effect without restarts. Constructing a BigtraceQueryClient is
  // cheap — it just stashes the endpoint string.
  private client(): BigtraceQueryClient {
    const setting = endpointStorage.get('bigtraceEndpoint');
    const endpoint = setting ? (setting.get() as string) : '';
    return new BigtraceQueryClient(endpoint);
  }

  async getAllHistory(): Promise<QueryExecution[]> {
    // No endpoint → return empty so the sidebar shows its "no queries
    // yet" empty state instead of a 404 from the static UI server.
    const setting = endpointStorage.get('bigtraceEndpoint');
    const endpoint = setting ? (setting.get() as string) : '';
    if (endpoint.trim() === '') return [];
    const list = await this.client().listQueryExecutions();
    const mapped = list.map(toQueryExecution);
    mapped.sort((a, b) => (b.startTime ?? 0) - (a.startTime ?? 0));
    return mapped;
  }

  async getMaterializedHistory(): Promise<QueryExecution[]> {
    return (await this.getAllHistory()).filter(
      (item) => item.materialized === true,
    );
  }

  async getNonMaterializedHistory(): Promise<QueryExecution[]> {
    return (await this.getAllHistory()).filter(
      (item) => item.materialized !== true,
    );
  }

  async deleteQuery(uuid: string): Promise<void> {
    await this.client().deleteQueryExecution(uuid);
  }
}

function toQueryExecution(raw: RawQueryExecution): QueryExecution {
  return {
    uuid: raw.queryUuid ?? '',
    status: raw.status ?? 'UNKNOWN',
    startTime: isoToEpochMs(raw.startTime),
    endTime: isoToEpochMs(raw.endTime),
    processedRows: raw.processedRows ?? 0,
    processedTraces: raw.processedTraces ?? 0,
    totalTraces: raw.totalTraces ?? 0,
    error: raw.error ?? raw.errorMessage,
    perfettoSql: raw.perfettoSql,
    limit: raw.limit,
    materialized: raw.materialized,
    tableName: raw.tableName,
    tableLink: raw.tableLink,
  };
}

export const queryHistoryStorage = new QueryHistoryStorage();
