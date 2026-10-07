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
import type {ExperimentFilterSpec, TracePreset} from './bigtrace_query_client';
import {encodeFilters} from './filter_encoding';

// What a tab is currently configured with, in the shape a preset declares it.
export interface PresetComparable {
  readonly sql: string;
  readonly traceFilters: readonly Filter[];
  // null = unchosen (the backend's default-visible columns).
  readonly traceMetadataColumns: readonly string[] | null;
  readonly traceOrderBy: string;
  // Ids + arm only; display names never take part in a comparison.
  readonly experimentFilter: ExperimentFilterSpec | undefined;
  readonly settings: ReadonlyArray<SettingFilter>;
}

// Both-ways equality: a filter on one side and none on the other is a
// mismatch, exactly like the trace filters.
function experimentFiltersEqual(
  a: ExperimentFilterSpec | undefined,
  b: ExperimentFilterSpec | undefined,
): boolean {
  if (a === undefined || b === undefined) return a === b;
  return (
    a.experimentId === b.experimentId &&
    a.controlId === b.controlId &&
    a.isTreatment === b.isTreatment
  );
}

// Whether `current` is exactly what the preset describes, so the launcher can
// tell which preset a tab came from: the setup, and the query if the preset
// carries one — a setup-only preset says nothing about the query.
export function presetMatches(
  preset: TracePreset,
  current: PresetComparable,
): boolean {
  const sql = preset.perfettoSql.trim();
  if (sql !== '' && current.sql.trim() !== sql) return false;
  return setupMatches(preset, current);
}

// Whether the setup — everything but the query — is consistent with what the
// preset describes: its selection, and every setting it names at that value.
// Settings it doesn't name are free, so a preset still matches after the user
// adds what it left out (a trace source, say). `materialized` and `limit` are
// run-time controls rather than configuration, so they're deliberately not
// compared.
export function setupMatches(
  preset: TracePreset,
  current: Omit<PresetComparable, 'sql'>,
): boolean {
  if (current.traceOrderBy !== (preset.traceOrderBy ?? '')) return false;
  if (
    !experimentFiltersEqual(current.experimentFilter, preset.experimentFilter)
  ) {
    return false;
  }
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

// Which settings read as booleans (off = "false") and which are run controls
// left out of the comparison, so a strict check can tell "off" from "on".
export interface SettingKinds {
  readonly booleanIds: ReadonlySet<string>;
  readonly ignoredIds: ReadonlySet<string>;
}

// Whether the current setup is exactly what applying the preset's setup would
// leave: its named settings at their values, and every other setting off —
// booleans false, the rest disabled — run controls aside. Strict where
// setupMatches is permissive, because the Settings form reads this back as
// "this tab runs with X's setup", which a setup with extras is not.
export function setupEquals(
  preset: TracePreset,
  current: Omit<PresetComparable, 'sql'>,
  kinds: SettingKinds,
): boolean {
  if (!setupMatches(preset, current)) return false;
  const named = new Set((preset.settings ?? []).map((s) => s.settingId));
  for (const entry of current.settings) {
    if (named.has(entry.settingId) || kinds.ignoredIds.has(entry.settingId)) {
      continue;
    }
    if (!kinds.booleanIds.has(entry.settingId)) return false;
    if (entry.values.length !== 1 || entry.values[0] !== 'false') return false;
  }
  return true;
}
