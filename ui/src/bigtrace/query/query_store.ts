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

// UI-display label for a wire status. IN_PROGRESS shows as "Running" (shorter,
// no underscore); transient UNKNOWN reads as "Starting".
export function statusDisplayLabel(status: string): string {
  if (status === 'IN_PROGRESS') return 'Running';
  if (status === 'UNKNOWN') return 'Starting';
  const s = status.replace(/_/g, ' ');
  return s.charAt(0) + s.slice(1).toLowerCase();
}

// Compact integer format (1.2K, 3.4M, 1.5B) for cramped UI spots — status bar,
// history sidebar. Precise value belongs in the surrounding tooltip. When
// rounding loses precision the result is prefixed "~" (e.g. 3,383,384 →
// "~3.4M") so users know to consult the tooltip.
const COMPACT_FORMATTER = new Intl.NumberFormat('en', {
  notation: 'compact',
  maximumFractionDigits: 1,
});
const COMPACT_SUFFIX_MULTIPLIER: Readonly<Record<string, number>> = {
  '': 1,
  'K': 1e3,
  'M': 1e6,
  'B': 1e9,
  'T': 1e12,
};
export function formatCompact(n: number): string {
  const compact = COMPACT_FORMATTER.format(n);
  // Reconstruct the value implied by the compact form; mismatch means rounded.
  let numericPart = '';
  let suffix = '';
  for (const p of COMPACT_FORMATTER.formatToParts(n)) {
    if (p.type === 'compact') {
      suffix = p.value;
    } else if (
      p.type === 'integer' ||
      p.type === 'decimal' ||
      p.type === 'fraction' ||
      p.type === 'minusSign'
    ) {
      numericPart += p.value;
    }
  }
  const multiplier = COMPACT_SUFFIX_MULTIPLIER[suffix] ?? 1;
  // Inputs are integer counts; round away float noise (3.4 * 1e6 ≠ 3.4e6).
  const reconstructed = Math.round(parseFloat(numericPart) * multiplier);
  return reconstructed === n ? compact : `~${compact}`;
}

import type {BigtraceColumnSchema} from './column_types';

export type QueryExecutionPhase = 'PREVIEW' | 'FULL_TABLE';

export function toQueryExecutionPhase(
  raw: string | undefined,
): QueryExecutionPhase | undefined {
  if (raw === 'PREVIEW' || raw === 'FULL_TABLE') return raw;
  return undefined;
}

const PHASE_RANK: Readonly<Record<QueryExecutionPhase, number>> = {
  PREVIEW: 1,
  FULL_TABLE: 2,
};

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
  phase?: QueryExecutionPhase;
  error?: string;
  perfettoSql?: string;
  limit?: number;
  materialized?: boolean;
  tableName?: string;
  tableLink?: string;
  schema?: ReadonlyArray<BigtraceColumnSchema>;
}

function stripEmptySchema(
  incoming: Partial<QueryExecution>,
): Partial<QueryExecution> {
  if (!('schema' in incoming)) return incoming;
  if (incoming.schema !== undefined && incoming.schema.length > 0) {
    return incoming;
  }
  const copy = {...incoming};
  delete copy.schema;
  return copy;
}

// Merges live polling with bulk history. Without the rule below, a history
// refresh during IN_PROGRESS would rewind processedRows: keep live progress
// unless incoming is terminal or higher.
export class QueryStore {
  private queries = new Map<string, QueryExecution>();

  getOrCreate(
    uuid: string,
    initialData?: Partial<QueryExecution>,
  ): QueryExecution {
    let obj = this.queries.get(uuid);
    if (obj === undefined) {
      obj = {
        uuid,
        status: 'UNKNOWN',
        processedRows: 0,
        processedTraces: 0,
        totalTraces: 0,
      };
      this.queries.set(uuid, obj);
    }
    if (initialData) {
      this.mergeInto(obj, initialData);
    }
    return obj;
  }

  // No-op if entry missing; getOrCreate first.
  update(uuid: string, updates: Partial<QueryExecution>): void {
    const obj = this.queries.get(uuid);
    if (obj === undefined) return;
    Object.assign(obj, stripEmptySchema(updates));
  }

  getAll(): QueryExecution[] {
    return Array.from(this.queries.values());
  }

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
    const inRank = incoming.phase ? PHASE_RANK[incoming.phase] : 0;
    const curRank = obj.phase ? PHASE_RANK[obj.phase] : 0;
    const stageAdvanced = inRank > curRank;
    const sameStageAndProgressed =
      inRank === curRank &&
      (incoming.processedRows ?? 0) >= obj.processedRows &&
      (incoming.processedTraces ?? 0) >= obj.processedTraces;

    // Listing endpoint clips perfettoSql/error and sends an empty schema; never
    // downgrade held values with shorter or empty ones.
    const patch: Partial<QueryExecution> = {...stripEmptySchema(incoming)};
    if (incomingIsTerminal) {
      patch.phase = undefined;
    }
    if (
      patch.perfettoSql !== undefined &&
      obj.perfettoSql !== undefined &&
      patch.perfettoSql.length < obj.perfettoSql.length
    ) {
      delete patch.perfettoSql;
    }
    if (
      obj.error !== undefined &&
      (patch.error === undefined || patch.error.length < obj.error.length)
    ) {
      delete patch.error;
    }

    if (
      !objIsLive ||
      incomingIsTerminal ||
      stageAdvanced ||
      sameStageAndProgressed
    ) {
      Object.assign(obj, patch);
      return;
    }

    // Stale snapshot: carry over static metadata only; preserve live counters.
    if (patch.tableLink !== undefined) obj.tableLink = patch.tableLink;
    if (patch.tableName !== undefined) obj.tableName = patch.tableName;
    if (patch.perfettoSql !== undefined) obj.perfettoSql = patch.perfettoSql;
    if (patch.limit !== undefined) obj.limit = patch.limit;
    if (patch.materialized !== undefined) obj.materialized = patch.materialized;
    if (patch.schema !== undefined) obj.schema = patch.schema;
  }
}

export const queryStore = new QueryStore();
