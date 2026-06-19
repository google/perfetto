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

// `link` is the clickable open-trace column; it leads every grid/picker.

export const LINK_COLUMN = 'link';

// Backend-internal row identifier present on every result; hidden unless the
// user explicitly adds it via the column picker.
export const ROW_ID_COLUMN = '_row_id';

// Hoist `link` to the front (keyed by name); no-op if absent or already first.
export function linkColumnFirst<T>(
  items: readonly T[],
  nameOf: (item: T) => string,
): T[] {
  const i = items.findIndex((it) => nameOf(it) === LINK_COLUMN);
  if (i <= 0) return [...items];
  return [items[i], ...items.slice(0, i), ...items.slice(i + 1)];
}

export function linkNameFirst(names: readonly string[]): string[] {
  return linkColumnFirst(names, (n) => n);
}

// Order: `link`, result columns, then `_`-prefixed metadata (stable, nothing dropped).
export function groupResultColumns(names: readonly string[]): string[] {
  const link = names.filter((n) => n === LINK_COLUMN);
  const ordinary = names.filter((n) => n !== LINK_COLUMN && !n.startsWith('_'));
  const meta = names.filter((n) => n !== LINK_COLUMN && n.startsWith('_'));
  return [...link, ...ordinary, ...meta];
}

// Resolve a results-grid column selection against the available columns, then
// group. null = show all but `_row_id`; a list is intersected (all-stale →
// back to the default). `_row_id` stays addable via the picker.
export function resolveResultColumns(
  chosen: readonly string[] | null,
  available: ReadonlyArray<string>,
): string[] {
  const defaults = available.filter((c) => c !== ROW_ID_COLUMN);
  if (chosen === null) {
    return groupResultColumns(defaults);
  }
  const known = new Set(available);
  const filtered = chosen.filter((c) => known.has(c));
  return groupResultColumns(filtered.length === 0 ? defaults : filtered);
}
