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

// Regex helpers for flamegraph filters. Frame names routinely contain regex
// metacharacters (`byte[]`, `operator()`, `MyClass$Nested`), so filters match
// literally by default and opt into regex with `/…/`.

// Escapes all regex metacharacters so the result matches |str| literally
// when embedded in a regular expression.
export function escapeRegex(str: string): string {
  return str.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

export interface UserFilterRegex {
  readonly pattern: string;
  readonly flags: '' | 'i';
}

// Interprets a user-typed flamegraph filter as a regular expression.
//
// Bare text is escaped and matched case-insensitively. `/…/` opts into a
// case-sensitive raw regex, while `/…/i` opts into a case-insensitive regex.
export function parseUserFilterRegex(filter: string): UserFilterRegex {
  if (filter.length >= 3 && filter.startsWith('/') && filter.endsWith('/i')) {
    return {pattern: filter.slice(1, -2), flags: 'i'};
  }
  if (filter.length >= 2 && filter.startsWith('/') && filter.endsWith('/')) {
    return {pattern: filter.slice(1, -1), flags: ''};
  }
  return {pattern: escapeRegex(filter), flags: 'i'};
}
