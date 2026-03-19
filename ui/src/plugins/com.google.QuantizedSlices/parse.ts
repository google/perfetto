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

// Pure parsing functions, no side effects.
// Shared between main thread and Web Worker.
//
// Every function here takes data in and returns data out.
// No mithril, no state, no DOM access.

import type {Slice, TraceEntry} from './models/types';
import {
  DEFAULT_COLUMN_CONFIG,
  DEFAULT_SLICE_FIELD_CONFIG,
} from './models/types';

// -- Field resolution --

// Resolves a field value from an object by trying a list of aliases.
// The fallback can be a static value or a factory function (for values like
// crypto.randomUUID() that must be unique per call). Factory functions are
// wrapped in {factory: () => T} to avoid ambiguity when T itself is a function.
type FallbackValue<T> = T | {factory: () => T};

export function resolveField<T>(
  obj: Record<string, unknown>,
  aliases: string[],
  fallback: FallbackValue<T>,
): T {
  for (const alias of aliases) {
    if (obj[alias] !== undefined) return obj[alias] as T;
  }
  if (
    typeof fallback === 'object' &&
    fallback !== null &&
    'factory' in fallback
  ) {
    return (fallback as {factory: () => T}).factory();
  }
  return fallback as T;
}

// -- Slice normalization --

export function normalizeSlice(raw: Record<string, unknown>): Slice {
  const cfg = DEFAULT_SLICE_FIELD_CONFIG;
  return {
    ts: resolveField(raw, cfg.ts.aliases, cfg.ts.fallback),
    dur: resolveField(raw, cfg.dur.aliases, cfg.dur.fallback),
    name: resolveField(raw, cfg.name.aliases, cfg.name.fallback),
    state: resolveField(raw, cfg.state.aliases, cfg.state.fallback),
    depth: resolveField(raw, cfg.depth.aliases, cfg.depth.fallback),
    io_wait: resolveField(raw, cfg.io_wait.aliases, cfg.io_wait.fallback),
    blocked_function: resolveField(
      raw,
      cfg.blocked_function.aliases,
      cfg.blocked_function.fallback,
    ),
  };
}

// -- Startup duration (ms -> ns conversion) --

const MS_ALIASES = new Set(['startup_dur_ms', 'startup_ms']);

function resolveStartupDur(obj: Record<string, unknown>): number {
  const cfg = DEFAULT_COLUMN_CONFIG;
  for (const alias of cfg.startup_dur.aliases) {
    if (obj[alias] !== undefined) {
      const val = parseFloat(String(obj[alias])) || 0;
      return MS_ALIASES.has(alias) ? val * 1e6 : val;
    }
  }
  return 0;
}

// -- Package name (handles JSON-encoded column) --

export function resolvePackageName(raw: Record<string, unknown>): string {
  const cfg = DEFAULT_COLUMN_CONFIG;
  const val = resolveField(
    raw,
    cfg.package_name.aliases,
    cfg.package_name.fallback,
  );
  if (typeof val === 'string' && val.startsWith('{')) {
    try {
      const parsed = JSON.parse(val) as Record<string, unknown>;
      if (typeof parsed.package_name === 'string') return parsed.package_name;
    } catch {
      /* not JSON, use as-is */
    }
  }
  return val;
}

// -- Array-of-arrays -> objects --

export function arrayOfArraysToObjects(
  arr: unknown[][],
): Record<string, unknown>[] {
  const headers = arr[0] as string[];
  return arr.slice(1).map((row) => {
    const obj: Record<string, unknown> = {};
    headers.forEach((h, i) => {
      if ((row as unknown[])[i] !== undefined) obj[h] = (row as unknown[])[i];
    });
    return obj;
  });
}

// -- UUID extraction --
// trace_address values are paths like "/path/to/uuid.pftrace.gz"
// Extract the UUID portion (basename without extension) if the value looks
// like a path. If it's already a bare UUID, return as-is.

const UUID_RE = /[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/i;

function extractUuid(val: string): string {
  if (!val || !val.includes('/')) return val;
  const match = val.match(UUID_RE);
  if (match) return match[0];
  // Fallback: use basename without extension
  const base = val.split('/').pop() || val;
  return base.replace(/\.\w+(\.\w+)*$/, '');
}

// -- Shared slice-field parsing --
// Handles string (JSON/base64), object-with-slices, or raw array inputs.

export function parseSlicesField(rawSlices: unknown): Slice[] | null {
  if (typeof rawSlices === 'string') {
    let decoded = rawSlices;
    if (!decoded.startsWith('[') && !decoded.startsWith('{')) {
      try {
        decoded = atob(decoded);
      } catch {
        return null;
      }
    }
    try {
      let parsed: unknown;
      try {
        parsed = JSON.parse(decoded);
      } catch {
        parsed = JSON.parse(repairJson(decoded));
      }
      const parsedObj = parsed as Record<string, unknown>;
      const arr = Array.isArray(parsed)
        ? (parsed as unknown[])
        : ((parsedObj.slices ?? parsedObj.data) as unknown[] | undefined);
      if (!Array.isArray(arr) || arr.length === 0) return null;
      return arr.map((s: unknown) =>
        normalizeSlice(s as Record<string, unknown>),
      );
    } catch {
      return null;
    }
  }
  if (Array.isArray(rawSlices)) {
    const slices = rawSlices.map((s: unknown) =>
      normalizeSlice(s as Record<string, unknown>),
    );
    return slices.length > 0 ? slices : null;
  }
  return null;
}

// -- Trace normalization --

export function normalizeTrace(
  raw: Record<string, unknown>,
): TraceEntry | null {
  const cfg = DEFAULT_COLUMN_CONFIG;
  const rawSlices = resolveField<unknown>(
    raw,
    cfg.slices.aliases,
    cfg.slices.fallback,
  );

  const slices = parseSlicesField(rawSlices);
  if (!slices) return null;

  // Collect extra fields (anything not a known column alias)
  const knownKeys = new Set([
    ...cfg.trace_uuid.aliases,
    ...cfg.package_name.aliases,
    ...cfg.startup_dur.aliases,
    ...cfg.slices.aliases,
  ]);
  const extra: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(raw)) {
    if (!knownKeys.has(k)) extra[k] = v;
  }

  return {
    trace_uuid: extractUuid(
      resolveField(raw, cfg.trace_uuid.aliases, cfg.trace_uuid.fallback),
    ),
    package_name: resolvePackageName(raw),
    startup_dur: resolveStartupDur(raw),
    slices,
    extra: Object.keys(extra).length > 0 ? extra : undefined,
  };
}

// -- JSON repair for truncated input --

export function repairJson(text: string): string {
  let result = text.trimEnd();
  let inStr = false;
  let escape = false;
  const stack: string[] = [];
  for (let i = 0; i < result.length; i++) {
    const ch = result[i];
    if (escape) {
      escape = false;
      continue;
    }
    if (ch === '\\' && inStr) {
      escape = true;
      continue;
    }
    if (ch === '"' && !escape) {
      inStr = !inStr;
      continue;
    }
    if (inStr) continue;
    if (ch === '[') stack.push(']');
    else if (ch === '{') stack.push('}');
    else if (ch === ']' || ch === '}') {
      if (stack.length > 0 && stack[stack.length - 1] === ch) stack.pop();
    }
  }
  if (inStr) result += '"';
  while (stack.length > 0) result += stack.pop();
  return result;
}

// -- Delimited (TSV/CSV) row parsing -- RFC 4180 compliant --

export function parseDelimitedRows(
  text: string,
  delimiter: string,
): string[][] {
  const rows: string[][] = [];
  let fields: string[] = [];
  let current = '';
  let inQ = false;
  let i = 0;

  while (i < text.length) {
    const ch = text[i];
    if (inQ) {
      if (ch === '"') {
        if (i + 1 < text.length && text[i + 1] === '"') {
          // Escaped quote ""
          current += '"';
          i += 2;
          continue;
        }
        // End of quoted field
        inQ = false;
        i++;
        continue;
      }
      current += ch;
      i++;
    } else {
      if (ch === '"' && current === '') {
        inQ = true;
        i++;
      } else if (ch === delimiter) {
        fields.push(current);
        current = '';
        i++;
      } else if (ch === '\n' || ch === '\r') {
        fields.push(current);
        current = '';
        if (ch === '\r' && i + 1 < text.length && text[i + 1] === '\n') i++;
        if (fields.some((f) => f.trim() !== '')) rows.push(fields);
        fields = [];
        i++;
      } else {
        current += ch;
        i++;
      }
    }
  }

  // Final row
  fields.push(current);
  if (fields.some((f) => f.trim() !== '')) rows.push(fields);
  return rows;
}

// -- Progress callback type --

export interface ParseProgress {
  message: string;
  current?: number;
  total?: number;
}

// -- High-level parse: JSON text -> TraceEntry[] --

export function parseJsonToTraces(
  text: string,
  onProgress?: (p: ParseProgress) => void,
): TraceEntry[] {
  onProgress?.({message: 'Parsing JSON...'});

  let parsed: unknown;
  try {
    parsed = JSON.parse(text);
  } catch {
    parsed = JSON.parse(repairJson(text));
  }

  if (Array.isArray(parsed) && parsed.length) {
    let items: unknown[] = parsed;

    // Array-of-arrays: [[headers], [row1], [row2], ...]
    if (
      Array.isArray(parsed[0]) &&
      parsed.length >= 2 &&
      (parsed[0] as unknown[]).every((h: unknown) => typeof h === 'string')
    ) {
      onProgress?.({message: 'Converting array-of-arrays...'});
      items = arrayOfArraysToObjects(parsed as unknown[][]);
    }

    const first = items[0];
    if (typeof first !== 'object' || first === null || Array.isArray(first)) {
      throw new Error('Expected array of objects or [headers, ...rows]');
    }

    const firstObj = first as Record<string, unknown>;

    // Detect: is this an array of slices or an array of traces?
    const looksLikeSlice =
      DEFAULT_SLICE_FIELD_CONFIG.ts.aliases.some(
        (a) => firstObj[a] !== undefined,
      ) &&
      DEFAULT_SLICE_FIELD_CONFIG.dur.aliases.some(
        (a) => firstObj[a] !== undefined,
      );
    const looksLikeTrace = DEFAULT_COLUMN_CONFIG.slices.aliases.some(
      (a) => firstObj[a] !== undefined,
    );

    if (looksLikeSlice && !looksLikeTrace) {
      onProgress?.({message: `Normalizing ${items.length} slices...`});
      const slices = items.map((s) =>
        normalizeSlice(s as Record<string, unknown>),
      );
      return [
        {
          trace_uuid: crypto.randomUUID(),
          package_name: 'unknown',
          startup_dur: 0,
          slices,
        },
      ];
    }

    if (looksLikeTrace) {
      const total = items.length;
      const traces: TraceEntry[] = [];
      for (let i = 0; i < total; i++) {
        if (i % 10 === 0) {
          onProgress?.({
            message: `Processing trace ${i + 1}/${total}...`,
            current: i,
            total,
          });
        }
        const t = normalizeTrace(items[i] as Record<string, unknown>);
        if (t) traces.push(t);
      }
      if (traces.length === 0) throw new Error('No valid traces in array');
      return traces;
    }

    throw new Error(
      'Array items need ts+dur (slices) or a slices/json/data column (traces)',
    );
  }

  if (typeof parsed === 'object' && parsed !== null) {
    const trace = normalizeTrace(parsed as Record<string, unknown>);
    if (trace) return [trace];
    throw new Error('Object must have a slices/json/data field');
  }

  throw new Error('Expected array or object');
}

// -- High-level parse: delimited text -> TraceEntry[] --

export function parseDelimitedToTraces(
  text: string,
  delimiter: string,
  onProgress?: (p: ParseProgress) => void,
): TraceEntry[] {
  onProgress?.({message: 'Parsing rows...'});
  const rows = parseDelimitedRows(text, delimiter);
  if (rows.length < 2) throw new Error('Need header + data rows');

  const headers = rows[0];
  const cfg = DEFAULT_COLUMN_CONFIG;
  const norm = (s: string): string =>
    s.toLowerCase().trim().replace(/\s+/g, '_');
  const findCol = (aliases: string[]): number => {
    for (const a of aliases) {
      const idx = headers.findIndex((h) => norm(h) === a.toLowerCase());
      if (idx >= 0) return idx;
    }
    return -1;
  };

  const slicesIdx = findCol(cfg.slices.aliases);
  const uuidIdx = findCol(cfg.trace_uuid.aliases);
  const pkgIdx = findCol(cfg.package_name.aliases);
  const durIdx = findCol(cfg.startup_dur.aliases);
  const durIsMs = durIdx >= 0 && MS_ALIASES.has(norm(headers[durIdx]));

  if (slicesIdx < 0) {
    throw new Error(`Need a column matching: ${cfg.slices.aliases.join(', ')}`);
  }

  const traces: TraceEntry[] = [];
  let parseErrors = 0;
  const total = rows.length - 1;

  for (let ri = 1; ri < rows.length; ri++) {
    if ((ri - 1) % 10 === 0) {
      onProgress?.({
        message: `Processing row ${ri}/${total}...`,
        current: ri - 1,
        total,
      });
    }

    const cols = rows[ri];
    if (!cols[slicesIdx]?.trim()) continue;

    try {
      const slices = parseSlicesField(cols[slicesIdx].trim());
      if (!slices) {
        parseErrors++;
        continue;
      }

      const extra: Record<string, unknown> = {};
      headers.forEach((h, idx) => {
        if (
          idx !== slicesIdx &&
          idx !== uuidIdx &&
          idx !== pkgIdx &&
          idx !== durIdx
        ) {
          if (cols[idx]?.trim()) extra[norm(h)] = cols[idx].trim();
        }
      });

      let pkgName =
        pkgIdx >= 0 && cols[pkgIdx]
          ? cols[pkgIdx].trim()
          : cfg.package_name.fallback.factory();
      if (pkgName.startsWith('{')) {
        try {
          const p = JSON.parse(pkgName) as Record<string, unknown>;
          if (typeof p.package_name === 'string') pkgName = p.package_name;
        } catch {
          /* not JSON */
        }
      }

      traces.push({
        trace_uuid: extractUuid(
          uuidIdx >= 0 && cols[uuidIdx]
            ? cols[uuidIdx].trim()
            : cfg.trace_uuid.fallback.factory(),
        ),
        package_name: pkgName,
        startup_dur:
          durIdx >= 0 && cols[durIdx]
            ? (parseFloat(cols[durIdx]) || 0) * (durIsMs ? 1e6 : 1)
            : 0,
        slices,
        extra: Object.keys(extra).length > 0 ? extra : undefined,
      });
    } catch {
      parseErrors++;
    }
  }

  if (!traces.length) {
    throw new Error(`No valid traces (${parseErrors} parse errors)`);
  }
  return traces;
}

// -- Unified entry point: auto-detect format --

export function parseText(
  text: string,
  onProgress?: (p: ParseProgress) => void,
): TraceEntry[] {
  text = text.trim();
  if (!text) return [];

  if (text.startsWith('[') || text.startsWith('{')) {
    return parseJsonToTraces(text, onProgress);
  }

  const firstNewline = text.indexOf('\n');
  const firstLine = firstNewline >= 0 ? text.slice(0, firstNewline) : text;
  if (firstLine.includes('\t')) {
    return parseDelimitedToTraces(text, '\t', onProgress);
  }
  if (firstLine.includes(',')) {
    return parseDelimitedToTraces(text, ',', onProgress);
  }

  // Fallback: try as JSON
  return parseJsonToTraces(text, onProgress);
}
