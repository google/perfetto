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

import {shortUuid} from '../../base/uuid';
import {LocalStorage} from '../../core/local_storage';
import type {TracePreset} from './bigtrace_query_client';
import {
  effectiveTabSettings,
  type BigTraceEditorTab,
} from '../pages/query_tabs_state';

// A preset the user saved in this browser. Same shape as a backend preset so
// both apply through one code path; `isLocal` only drives the launcher's
// rename / delete affordances.
export interface LocalPreset extends TracePreset {
  readonly isLocal: true;
}

const LOCAL_PRESETS_KEY = 'bigtraceLocalPresets';
const PRESETS_FIELD = 'presets';
const LOCAL_ID_PREFIX = 'local:';

// Category local presets land in when the user doesn't pick one. Groups them
// together in the launcher, away from the backend catalog's CUJs.
export const DEFAULT_LOCAL_CATEGORY = 'My presets';

export function isLocalPresetId(id: string): boolean {
  return id.startsWith(LOCAL_ID_PREFIX);
}

// Drop anything that isn't a usable preset — the store is user-editable
// localStorage, so a hand-edited or half-written entry must not break the
// launcher.
function parsePresets(raw: unknown): LocalPreset[] {
  if (!Array.isArray(raw)) return [];
  const out: LocalPreset[] = [];
  for (const entry of raw) {
    if (typeof entry !== 'object' || entry === null) continue;
    const p = entry as Partial<LocalPreset>;
    if (typeof p.id !== 'string' || p.id === '') continue;
    if (typeof p.name !== 'string' || p.name === '') continue;
    if (typeof p.perfettoSql !== 'string') continue;
    out.push({...(p as LocalPreset), isLocal: true});
  }
  return out;
}

// Presets the user saved locally. Backed by one localStorage key; the launcher
// merges these with the backend catalog.
class LocalPresetStore {
  private readonly storage = new LocalStorage(LOCAL_PRESETS_KEY);

  list(): LocalPreset[] {
    return parsePresets(this.storage.load()[PRESETS_FIELD]);
  }

  get(id: string): LocalPreset | undefined {
    return this.list().find((p) => p.id === id);
  }

  // Case-insensitive so "Jank" doesn't sit next to "jank"; the launcher uses
  // this to offer an overwrite instead of silently creating a twin.
  findByName(name: string): LocalPreset | undefined {
    const wanted = name.trim().toLowerCase();
    return this.list().find((p) => p.name.trim().toLowerCase() === wanted);
  }

  // Insert, or replace in place when the id already exists (overwrite keeps
  // the preset's position in the launcher).
  save(preset: LocalPreset): LocalPreset {
    const stored: LocalPreset = {
      ...preset,
      id: preset.id === '' ? newLocalPresetId() : preset.id,
      isLocal: true,
    };
    const presets = this.list();
    const idx = presets.findIndex((p) => p.id === stored.id);
    if (idx >= 0) presets[idx] = stored;
    else presets.push(stored);
    this.write(presets);
    return stored;
  }

  rename(id: string, name: string): void {
    const presets = this.list();
    const idx = presets.findIndex((p) => p.id === id);
    if (idx < 0) return;
    presets[idx] = {...presets[idx], name};
    this.write(presets);
  }

  remove(id: string): void {
    this.write(this.list().filter((p) => p.id !== id));
  }

  private write(presets: ReadonlyArray<LocalPreset>): void {
    try {
      this.storage.save({[PRESETS_FIELD]: presets});
    } catch {
      // QuotaExceededError — the presets in memory still apply for this
      // session; the next successful write persists them.
    }
  }
}

export const localPresetStore = new LocalPresetStore();

export function newLocalPresetId(): string {
  return `${LOCAL_ID_PREFIX}${shortUuid()}`;
}

// Capture a tab's configuration as a preset. Settings are the EFFECTIVE ones
// (per-tab overrides layered over the backend defaults), so applying the preset
// later reproduces this run without depending on what else is registered.
export function presetFromTab(
  tab: BigTraceEditorTab,
  opts: {
    readonly name: string;
    readonly category?: string;
    readonly description?: string;
    // Set to overwrite an existing local preset instead of creating one.
    readonly id?: string;
  },
): LocalPreset {
  const cols = tab.traceMetadataColumns;
  return {
    isLocal: true,
    id: opts.id ?? newLocalPresetId(),
    name: opts.name,
    category: opts.category ?? DEFAULT_LOCAL_CATEGORY,
    description: opts.description ?? '',
    icon: 'bookmark',
    perfettoSql: tab.editorText,
    settings: effectiveTabSettings(tab).map((s) => ({
      settingId: s.settingId,
      values: [...s.values],
      category: s.category,
    })),
    traceFilters: [...tab.traceFilters],
    // null means "whatever the backend flags default-visible"; the wire says
    // that with an empty list, and applying maps it back to null.
    traceMetadataColumns: cols === null ? [] : [...cols],
    traceOrderBy: tab.traceOrderBy,
    limit: tab.limit,
    materialized: tab.materialize,
  };
}
