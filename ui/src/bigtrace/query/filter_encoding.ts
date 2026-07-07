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

import type {Filter} from '../../components/widgets/datagrid/model';

// Coerce one scalar to its always-strings wire form: null stays null, strings
// pass through, number/bigint/boolean via String(...) to preserve int64
// precision past Number.MAX_SAFE_INTEGER.
function coerceScalar(v: unknown): unknown {
  if (v === null || typeof v === 'string') return v;
  if (
    typeof v === 'number' ||
    typeof v === 'bigint' ||
    typeof v === 'boolean'
  ) {
    return String(v);
  }
  return v;
}

// Returns a fresh `Filter[]` with every scalar coerced to the always-strings
// wire form. Used to ship filters in request bodies (`/trace_metadata`
// `filters`, `/execute_*` `trace_filters`).
export function coerceFiltersForWire(filters: ReadonlyArray<Filter>): Filter[] {
  return filters.map((f) => {
    const out: Record<string, unknown> = {field: f.field, op: f.op};
    if ('value' in f) {
      const v = (f as {value: unknown}).value;
      out.value = Array.isArray(v) ? v.map(coerceScalar) : coerceScalar(v);
    }
    return out as unknown as Filter;
  });
}

// Stable canonical-key JSON form of `filters`. Used by the data sources as a
// change-detection key, so keys are sorted to make equivalent filters hash
// identically regardless of construction order. Shares value coercion with the
// body-shipping path via `coerceFiltersForWire`.
export function encodeFilters(filters: ReadonlyArray<Filter>): string {
  return JSON.stringify(coerceFiltersForWire(filters), (_key, value) => {
    if (typeof value === 'object' && value !== null && !Array.isArray(value)) {
      const sorted: Record<string, unknown> = {};
      for (const k of Object.keys(value).sort()) {
        sorted[k] = (value as Record<string, unknown>)[k];
      }
      return sorted;
    }
    return value;
  });
}
