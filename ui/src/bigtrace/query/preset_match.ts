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

import {arrayEquals} from '../../base/array_utils';
import type {Filter} from '../../components/widgets/datagrid/model';
import type {SettingFilter} from '../settings/settings_types';
import type {TracePreset} from './bigtrace_query_client';
import {encodeFilters} from './filter_encoding';

// What a tab is currently configured with, in the shape a preset declares it.
export interface PresetComparable {
  readonly sql: string;
  readonly traceFilters: readonly Filter[];
  // null = unchosen (the backend's default-visible columns).
  readonly traceMetadataColumns: readonly string[] | null;
  readonly traceOrderBy: string;
  readonly settings: ReadonlyArray<SettingFilter>;
}

// Whether `current` is exactly what the preset describes, so the launcher can
// highlight the preset a tab came from. `materialized` and `limit` are run-time
// controls rather than configuration, so they're deliberately not compared.
export function presetMatches(
  preset: TracePreset,
  current: PresetComparable,
): boolean {
  if (current.sql.trim() !== preset.perfettoSql.trim()) return false;
  if (current.traceOrderBy !== (preset.traceOrderBy ?? '')) return false;
  // Compared by canonical key-sorted encoding, so a different key order in an
  // otherwise identical filter still matches.
  if (
    encodeFilters(current.traceFilters) !==
    encodeFilters(preset.traceFilters ?? [])
  ) {
    return false;
  }
  const cols = preset.traceMetadataColumns ?? [];
  const wantCols = cols.length ? cols : null;
  const curCols = current.traceMetadataColumns;
  if (curCols === null || wantCols === null) {
    if (curCols !== wantCols) return false;
  } else if (!arrayEquals([...curCols], [...wantCols])) {
    return false;
  }
  // Every option the preset states must equal the effective one; settings it
  // doesn't mention are free.
  for (const s of preset.settings ?? []) {
    const cur = current.settings.find((e) => e.settingId === s.settingId);
    if (cur === undefined || !arrayEquals(cur.values, [...s.values])) {
      return false;
    }
  }
  return true;
}
