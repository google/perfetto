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

// Preset library loader.
//
// Fetches `music_synth_presets.json` once at page load, converts the
// snake_case field names to camelCase (protobufjs expects camelCase), and
// provides a categorized/searchable API.

import protos from '../../protos';
import {assetSrc} from '../../base/assets';

const PRESETS_URL = 'assets/sound_synth/music_synth_presets.json';

export interface PresetEntry {
  readonly name: string;
  readonly category: string;
  readonly description: string;
  /**
   * Already-decoded SynthPatch. The TestPatternSource module is still in
   * here — it's stripped at import time by patch_state.importPresetAsInstrument
   * (NOT by the loader).
   */
  readonly patch: protos.ISynthPatch;
}

export interface PresetLibrary {
  all(): ReadonlyArray<PresetEntry>;
  byCategory(): Map<string, PresetEntry[]>;
  categories(): string[];
  search(query: string): PresetEntry[];
  findByName(name: string): PresetEntry | null;
}

/**
 * Recursively convert snake_case object keys to camelCase.
 * Leaves array and primitive values alone; only rewrites object keys.
 * Enum values that were emitted as uppercase strings (e.g. "ARPEGGIO")
 * are left untouched — protobufjs accepts them.
 */
function snakeToCamelDeep(obj: unknown): unknown {
  if (Array.isArray(obj)) {
    return obj.map(snakeToCamelDeep);
  }
  if (obj !== null && typeof obj === 'object') {
    const out: Record<string, unknown> = {};
    for (const [key, value] of Object.entries(obj)) {
      const camelKey = key.replace(/_([a-z])/g,
        (_, c: string) => c.toUpperCase());
      out[camelKey] = snakeToCamelDeep(value);
    }
    return out;
  }
  return obj;
}

let cachedLibrary: PresetLibrary | null = null;
let inflight: Promise<PresetLibrary> | null = null;

/**
 * Load the preset library (cached). Fetches the JSON on first call and
 * parses each preset into a SynthPatch via protobufjs.
 */
export async function loadPresetLibrary(): Promise<PresetLibrary> {
  if (cachedLibrary) return cachedLibrary;
  if (inflight) return inflight;

  inflight = (async () => {
    const resp = await fetch(assetSrc(PRESETS_URL));
    if (!resp.ok) {
      throw new Error(
        `Failed to load preset library: ${resp.status} ${resp.statusText}`,
      );
    }
    const raw = await resp.json();
    const entries: PresetEntry[] = [];

    const presets = Array.isArray(raw?.presets) ? raw.presets : [];
    for (const item of presets) {
      try {
        const camelPatch = snakeToCamelDeep(item.patch) as object;
        const patch = protos.SynthPatch.fromObject(camelPatch);
        entries.push({
          name: String(item.name ?? ''),
          category: String(item.category ?? 'misc'),
          description: String(item.description ?? ''),
          patch,
        });
      } catch (e) {
        // Skip presets that fail to parse. Don't crash the whole library.
        console.warn(
          `Failed to parse preset ${item?.name ?? '<unknown>'}:`, e);
      }
    }

    // Sort categories in a stable order that reflects the preset mix.
    const categoryOrder = [
      'drum', 'bass', 'lead', 'pad', 'fx', 'strings', 'organ',
    ];
    entries.sort((a, b) => {
      const ai = categoryOrder.indexOf(a.category);
      const bi = categoryOrder.indexOf(b.category);
      if (ai !== bi) return (ai < 0 ? 999 : ai) - (bi < 0 ? 999 : bi);
      return a.name.localeCompare(b.name);
    });

    cachedLibrary = buildLibrary(entries);
    return cachedLibrary;
  })();

  try {
    return await inflight;
  } finally {
    inflight = null;
  }
}

function buildLibrary(entries: PresetEntry[]): PresetLibrary {
  const byCat = new Map<string, PresetEntry[]>();
  for (const e of entries) {
    const list = byCat.get(e.category) ?? [];
    list.push(e);
    byCat.set(e.category, list);
  }
  const cats = Array.from(byCat.keys());

  return {
    all: () => entries,
    byCategory: () => byCat,
    categories: () => cats,
    search: (query: string) => {
      const q = query.trim().toLowerCase();
      if (!q) return entries;
      return entries.filter(
        (e) =>
          e.name.toLowerCase().includes(q) ||
          e.category.toLowerCase().includes(q) ||
          e.description.toLowerCase().includes(q),
      );
    },
    findByName: (name: string) =>
      entries.find((e) => e.name === name) ?? null,
  };
}
