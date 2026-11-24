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

import type {SqlValue} from '../../../trace_processor/query_result';
import type {QuickFilter} from './model';

/**
 * Returns true if the quick filter should be applied, i.e. it has non-empty text.
 */
export function isQuickFilterActive(
  quickFilter: QuickFilter | undefined,
): quickFilter is QuickFilter {
  return quickFilter !== undefined && quickFilter.text !== '';
}

/**
 * Returns true if `value` matches the quick filter `text`.
 *
 * The rules are:
 * 1. The value, converted to text, contains the filter text.
 * 2. Upper/lowercase is ignored for A-Z only.
 * 3. Nulls and blobs never match.
 * An empty filter text matches nothing.
 *
 * Keep in sync with quickFilterToSql() in sql_utils.ts, which implements the
 * same rules in SQL.
 */
export function valueMatchesQuickFilter(
  value: SqlValue,
  text: string,
): boolean {
  if (text === '' || value === null || value instanceof Uint8Array) {
    return false;
  }
  return asciiLowerCase(String(value)).includes(asciiLowerCase(text));
}

// Lowercases A-Z only, to match the SQL side (which only expands A-Z into
// case-insensitive glob character classes).
function asciiLowerCase(s: string): string {
  return s.replace(/[A-Z]/g, (c) => c.toLowerCase());
}
