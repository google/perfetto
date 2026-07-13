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
// metacharacters (`byte[]`, `operator()`, Java lambda `$` names), which
// break or silently mis-match when interpolated into REGEXP patterns.

// Escapes all regex metacharacters so the result matches |str| literally
// when embedded in a regular expression. Used when a known-literal frame
// name is turned into a filter (e.g. via the node menu).
export function escapeRegex(str: string): string {
  return str.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

// Rewrites every bare `[]` in a user-typed filter into `\[\]`.
//
// Filters like `byte[]` or `.*Object[]` are common for Java heap dumps, but
// an empty character class is either a compile error (RE2, PCRE2) or matches
// nothing (ECMAScript). No working pattern can contain a bare `[]`, so this
// rewrite only revives dead patterns and never changes a valid one.
export function escapeRegexEmptyBrackets(pattern: string): string {
  let res = '';
  let inClass = false;
  for (let i = 0; i < pattern.length; i++) {
    const c = pattern[i];
    if (c === '\\' && i + 1 < pattern.length) {
      res += c + pattern[i + 1];
      i++;
    } else if (!inClass && c === '[') {
      if (pattern[i + 1] === ']') {
        res += '\\[\\]';
        i++;
      } else {
        inClass = true;
        res += c;
      }
    } else {
      if (inClass && c === ']') {
        inClass = false;
      }
      res += c;
    }
  }
  return res;
}
