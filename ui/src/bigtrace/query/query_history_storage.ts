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

import {getBigtraceEndpoint} from '../settings/endpoint_storage';
import {BigtraceQueryClient} from './bigtrace_query_client';
import type {QueryExecution} from './query_store';
import type {Filter} from '../../components/widgets/datagrid/model';
import type {SettingFilter} from '../settings/settings_types';

// Wire shape from /query_executions[*].
// Times are ISO-8601; `readonly` marks the wire boundary.
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
  // Submit-time snapshot — what the query ran with. Echoed on the full GET
  // /query_executions/{uuid}; omitted from :status and the list response.
  // `settings` uses the camelCase wire shape (`settingId`); convert with
  // snapshotSettingsToFilters before handing to the settings layer.
  readonly settings?: ReadonlyArray<SnapshotSettingEntry>;
  readonly traceFilters?: ReadonlyArray<Filter>;
  readonly traceMetadataColumns?: ReadonlyArray<string>;
  readonly traceOrderBy?: string;
  readonly traceLimit?: number;
}

// Snapshot settings as echoed on the wire. Responses are camelCase (like every
// other response field — queryUuid, perfettoSql, …), so the setting id arrives
// as `settingId`. `values` is permissive — the wire echoes back whatever the
// client sent. Convert to SettingFilter via snapshotSettingsToFilters.
export interface SnapshotSettingEntry {
  readonly settingId: string;
  readonly values: ReadonlyArray<string | number | boolean | null>;
  readonly category: string;
}

// Convert the wire snapshot settings into SettingFilter[] (string values) so
// the settings layer sees the shape it serialized. Skips malformed entries;
// coerces non-string values back to strings (null → '').
export function snapshotSettingsToFilters(
  raw: ReadonlyArray<SnapshotSettingEntry> | undefined,
): SettingFilter[] {
  if (!raw) return [];
  const out: SettingFilter[] = [];
  for (const s of raw) {
    if (
      typeof s?.settingId !== 'string' ||
      !Array.isArray(s?.values) ||
      typeof s?.category !== 'string'
    ) {
      continue;
    }
    out.push({
      settingId: s.settingId,
      values: s.values.map((v) => (v === null ? '' : String(v))),
      category: s.category as SettingFilter['category'],
    });
  }
  return out;
}

// ISO-8601 → epoch ms; invalid/missing → undefined (never NaN).
export function isoToEpochMs(iso: string | undefined): number | undefined {
  if (iso === undefined) return undefined;
  const ms = new Date(iso).getTime();
  return Number.isFinite(ms) ? ms : undefined;
}

export class QueryHistoryStorage {
  // Fresh client per call so endpoint changes apply without restart.
  private client(): BigtraceQueryClient {
    return new BigtraceQueryClient(getBigtraceEndpoint());
  }

  async getAllHistory(): Promise<QueryExecution[]> {
    // No endpoint → empty, so the sidebar shows its empty state, not a 404.
    if (getBigtraceEndpoint().trim() === '') return [];
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

  // The listing endpoint clips perfettoSql; the per-uuid endpoint returns the
  // full text. Use this on demand (e.g. when the user expands a clamped SQL
  // preview in the history sidebar).
  async fetchFullSql(uuid: string): Promise<string | undefined> {
    const raw = await this.client().getQueryExecution(uuid);
    return raw.perfettoSql;
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
