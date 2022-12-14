/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

enum EscapeFlag {
  CaseInsensitive = 1,
  MatchAny = 2,
}

function escape(s: string, flags?: number): string {
  flags = flags === undefined ? 0 : flags;
  // See https://www.sqlite.org/lang_expr.html#:~:text=A%20string%20constant
  s = s.replace(/\'/g, '\'\'');
  s = s.replace(/\[/g, '[[]');
  if (flags & EscapeFlag.CaseInsensitive) {
    s = s.replace(/[a-zA-Z]/g, (m) => {
      const lower = m.toLowerCase();
      const upper = m.toUpperCase();
      return `[${lower}${upper}]`;
    });
  }
  s = s.replace(/\?/g, '[?]');
  s = s.replace(/\*/g, '[*]');
  if (flags & EscapeFlag.MatchAny) {
    s = `*${s}*`;
  }
  s = `'${s}'`;
  return s;
}

export function escapeQuery(s: string): string {
  return escape(s);
}

export function escapeSearchQuery(s: string): string {
  return escape(s, EscapeFlag.CaseInsensitive | EscapeFlag.MatchAny);
}

export function escapeGlob(s: string): string {
  // For globs we are only preoccupied by mismatching single quotes.
  s = s.replace(/\'/g, '\'\'');
  return `'*${s}*'`;
}
