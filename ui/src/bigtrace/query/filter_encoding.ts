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

import {Filter} from '../../components/widgets/datagrid/model';

/**
 * Wire encoder for the BigTrace `:fetch_results?filter=...` query
 * parameter. Lives in its own module so two consumers — the HTTP
 * client (`BigtraceQueryClient.fetchResults`) and the
 * `BigtraceAsyncDataSource` (which uses the same string for cheap
 * change-detection equality) — share one source of truth for the
 * encoding rules. If either consumer ever drifted from the other on
 * the wire shape, the data source's `currentFilterKey` equality
 * would silently disagree with what's actually on the URL.
 *
 * Wire shape: `value` is always a JSON `string` (or absent / `null`).
 * Numbers, booleans, and bigints are coerced via `String(...)` so the
 * wire is uniformly typed and int64 precision survives the round-trip
 * (`(1700000000000000000n).toString() === "1700000000000000000"`,
 * lossless). The backend's DuckDB binder coerces the string to the
 * column's actual type at execute time — `WHERE big = ?` with the
 * string `"1700000000000000000"` matches the BIGINT row exactly.
 *
 * `null` passes through as JSON null (distinct from the string
 * `"null"`). `Uint8Array` values aren't reachable through the bigtrace
 * path today; if BLOB filtering is ever wanted, the encoder, the
 * response decoder, and the backend would all need a coordinated
 * encoding choice — handle that here when the time comes.
 *
 * Object keys are sorted so two `Filter` objects built via different
 * construction paths (e.g. `{field, op, value}` vs `{op, value, field}`
 * from a spread) hash to the same string. Without this, the data
 * source's `currentFilterKey` equality would trigger spurious
 * refetches.
 */
export function encodeFilters(filters: ReadonlyArray<Filter>): string {
  return JSON.stringify(filters, (_key, value) => {
    if (typeof value === 'object' && value !== null) {
      // Arrays — including the top-level filter array and the
      // value-arrays of `in`/`not in` ops — keep their natural
      // structure for JSON.stringify to recurse into; the replacer
      // is reapplied to each element.
      if (Array.isArray(value)) return value;
      // Plain objects (filter entries) get keys sorted so equality
      // keys are stable across construction-order differences.
      const sorted: Record<string, unknown> = {};
      for (const k of Object.keys(value).sort()) {
        sorted[k] = (value as Record<string, unknown>)[k];
      }
      return sorted;
    }
    // Coerce non-null, non-string primitives to strings. `null`
    // stays JSON null; strings pass through unchanged.
    if (value !== null && typeof value !== 'string') {
      return String(value);
    }
    return value;
  });
}
