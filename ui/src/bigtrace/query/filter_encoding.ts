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

// Wire encoder for `:fetch_results?filter=...`, shared by the HTTP client
// and `BigtraceAsyncDataSource` (which compares the encoded string for
// change-detection — drift would silently break equality).
//
// Values are coerced to strings (preserves int64 precision; DuckDB's binder
// coerces back to the column's type). `null` passes through as JSON null.
// Object keys are sorted so equivalent filters hash to the same string.
export function encodeFilters(filters: ReadonlyArray<Filter>): string {
  return JSON.stringify(filters, (_key, value) => {
    if (typeof value === 'object' && value !== null) {
      if (Array.isArray(value)) return value;
      // Sort keys for stable equality across construction order.
      const sorted: Record<string, unknown> = {};
      for (const k of Object.keys(value).sort()) {
        sorted[k] = (value as Record<string, unknown>)[k];
      }
      return sorted;
    }
    if (value !== null && typeof value !== 'string') {
      return String(value);
    }
    return value;
  });
}
