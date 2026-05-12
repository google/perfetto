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

// Statuses after which polling stops.
export const TERMINAL_STATUSES: ReadonlySet<string> = new Set([
  'SUCCESS',
  'FAILED',
  'CANCELLED',
]);

// UI-side execution record; times are epoch ms (ISO→epoch happens at the
// wire boundary in QueryHistoryStorage).
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

// Merges live polling (`getStatus`) with bulk history (`listQueryExecutions`).
// Without the rule below, a history refresh during IN_PROGRESS would rewind
// processedRows: keep live progress unless incoming is terminal or higher.
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

  // No-op if entry missing; getOrCreate first.
  update(uuid: string, updates: Partial<QueryExecution>): void {
    const obj = this.queries.get(uuid);
    if (obj === undefined) return;
    Object.assign(obj, updates);
  }

  getAll(): QueryExecution[] {
    return Array.from(this.queries.values());
  }

  // Test seam.
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

    // Listing endpoint clips perfettoSql/error; never downgrade the held
    // longer string with a shorter one.
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

    // Stale snapshot: carry over static metadata only; preserve live counters.
    if (patch.tableLink !== undefined) obj.tableLink = patch.tableLink;
    if (patch.tableName !== undefined) obj.tableName = patch.tableName;
    if (patch.perfettoSql !== undefined) obj.perfettoSql = patch.perfettoSql;
    if (patch.limit !== undefined) obj.limit = patch.limit;
    if (patch.materialized !== undefined) obj.materialized = patch.materialized;
  }
}

export const queryStore = new QueryStore();
