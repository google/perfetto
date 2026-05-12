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

// Wire-level statuses for which a query is final and no further
// polling is needed.
export const TERMINAL_STATUSES: ReadonlySet<string> = new Set([
  'SUCCESS',
  'FAILED',
  'CANCELLED',
]);

// In-memory representation of a single query execution as the UI thinks of
// it. Times are epoch milliseconds (numbers), not ISO strings — the
// QueryHistoryStorage layer is responsible for ISO-to-epoch conversion at
// the wire boundary so this layer can do plain arithmetic.
export interface QueryExecution {
  uuid: string;
  status: string;
  startTime?: number;
  endTime?: number;
  processedRows: number;
  processedTraces: number;
  totalTraces: number;
  error?: string;
  perfettoSql?: string;
  limit?: number;
  materialized?: boolean;
  tableName?: string;
  tableLink?: string;
}

// Bookkeeping for live queries the UI is polling.
//
// The store has to reconcile two sources of truth:
//   1. Live status polling (`getStatus`) running on the UI thread, which
//      pushes monotonically-increasing progress counters as a query runs.
//   2. The history list (`listQueryExecutions`), which is fetched in bulk
//      and may carry STALE counters for queries that have since advanced.
//
// Without a merge rule, a history refresh landing while a query is IN_PROGRESS
// would silently wind back `processedRows` and confuse the UI. The rule
// below preserves live progress unless the incoming snapshot is itself
// terminal or carries a higher row count.
export class QueryStore {
  private queries = new Map<string, QueryExecution>();

  getOrCreate(
    uuid: string,
    initialData?: Partial<QueryExecution>,
  ): QueryExecution {
    if (!this.queries.has(uuid)) {
      this.queries.set(uuid, {
        uuid,
        status: 'UNKNOWN',
        processedRows: 0,
        processedTraces: 0,
        totalTraces: 0,
        ...initialData,
      });
    }
    const obj = this.queries.get(uuid)!;
    if (initialData) {
      this.mergeInto(obj, initialData);
    }
    return obj;
  }

  // Apply a partial update to an existing entry. No-op if the entry doesn't
  // exist yet — callers must getOrCreate first.
  update(uuid: string, updates: Partial<QueryExecution>): void {
    const obj = this.queries.get(uuid);
    if (obj === undefined) return;
    Object.assign(obj, updates);
  }

  getAll(): QueryExecution[] {
    return Array.from(this.queries.values());
  }

  // Test seam: drop everything. Production callers shouldn't need this.
  clear(): void {
    this.queries.clear();
  }

  private mergeInto(
    obj: QueryExecution,
    incoming: Partial<QueryExecution>,
  ): void {
    const incomingIsTerminal =
      incoming.status !== undefined && TERMINAL_STATUSES.has(incoming.status);
    const objIsLive = obj.status === 'IN_PROGRESS' || obj.status === 'UNKNOWN';
    const rowCountIncreased =
      (incoming.processedRows ?? 0) >= obj.processedRows;

    // Sanitize the incoming patch before applying. The listing endpoint
    // returns clipped perfettoSql / errorMessage; the per-UUID detail
    // endpoint returns the full text. If the store already holds the full
    // version, a later list refresh must not downgrade it back to the
    // clipped text. Heuristic: never replace a longer string with a
    // shorter one for these fields.
    const patch: Partial<QueryExecution> = {...incoming};
    if (
      patch.perfettoSql !== undefined &&
      obj.perfettoSql !== undefined &&
      patch.perfettoSql.length < obj.perfettoSql.length
    ) {
      delete patch.perfettoSql;
    }
    if (
      patch.error !== undefined &&
      obj.error !== undefined &&
      patch.error.length < obj.error.length
    ) {
      delete patch.error;
    }

    if (!objIsLive || incomingIsTerminal || rowCountIncreased) {
      Object.assign(obj, patch);
      return;
    }

    // The incoming snapshot is staler than what we have. Only carry over
    // static metadata; preserve live progress counters.
    if (patch.tableLink !== undefined) obj.tableLink = patch.tableLink;
    if (patch.tableName !== undefined) obj.tableName = patch.tableName;
    if (patch.perfettoSql !== undefined) obj.perfettoSql = patch.perfettoSql;
    if (patch.limit !== undefined) obj.limit = patch.limit;
    if (patch.materialized !== undefined) obj.materialized = patch.materialized;
  }
}

export const queryStore = new QueryStore();
