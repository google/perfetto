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

import type {AreaSelection} from '../../public/selection';
import {sqlValueToSqliteString} from '../../trace_processor/sql_utils';

export const STACK_SAMPLE_TRACK_KIND = 'StackSampleTrack';

// Which profiler sessions an execution context covers: every session, only
// samples without a session, or one specific session.
export type SessionSpec = 'all' | 'null' | number;

// One execution context contributing samples to an area selection: a thread
// or a process, optionally narrowed to a profiler session. This is the entry
// granularity of the stack-sample flamegraph collection — the same
// granularity the selected tracks have.
export interface ContextEntry {
  readonly scope: 'utid' | 'upid';
  readonly id: number;
  readonly session: SessionSpec;
}

// Extracts the distinct execution contexts for `source` from the selected
// tracks' tags (the same walk the merged flamegraph used to build its SQL
// constraints).
export function contextEntriesForSelection(
  selection: AreaSelection,
  source: string,
): ContextEntry[] {
  const entries = new Map<string, ContextEntry>();
  for (const trackInfo of selection.tracks) {
    const tags = trackInfo?.tags;
    if (
      !tags?.kinds?.includes(STACK_SAMPLE_TRACK_KIND) ||
      tags.stackSampleSource !== source
    ) {
      continue;
    }
    let scope: 'utid' | 'upid';
    let id: number;
    if (tags.utid !== undefined) {
      scope = 'utid';
      id = Number(tags.utid);
    } else if (tags.upid !== undefined) {
      scope = 'upid';
      id = Number(tags.upid);
    } else {
      continue;
    }
    let session: SessionSpec;
    if (tags.stackSampleSessionId !== undefined) {
      session = Number(tags.stackSampleSessionId);
    } else if (tags.stackSampleNullSession === true) {
      session = 'null';
    } else {
      session = 'all';
    }
    const entry: ContextEntry = {scope, id, session};
    entries.set(contextKey(entry), entry);
  }
  return [...entries.values()];
}

// Stable string identity of a context, used as the collection entry key.
// Examples: 'utid=12', 'upid=4;session=3', 'utid=7;session=null'.
export function contextKey(entry: ContextEntry): string {
  const base = `${entry.scope}=${entry.id}`;
  if (entry.session === 'all') return base;
  return `${base};session=${entry.session}`;
}

export function parseContextKey(key: string): ContextEntry {
  const [context, session] = key.split(';');
  const [scope, id] = context.split('=');
  if (scope !== 'utid' && scope !== 'upid') {
    throw new Error(`Invalid context key: ${key}`);
  }
  let sessionSpec: SessionSpec = 'all';
  if (session !== undefined) {
    const value = session.slice('session='.length);
    sessionSpec = value === 'null' ? 'null' : Number(value);
  }
  return {scope, id: Number(id), session: sessionSpec};
}

// The SQL condition selecting one context's samples, over the joins
// `stack_sample p LEFT JOIN stack_sample_task_context tc LEFT JOIN thread t`.
// Byte-compatible with the fragments the merged flamegraph used to build.
export function contextSqlCondition(
  source: string,
  entry: ContextEntry,
): string {
  const parts = [`p.source = ${sqlValueToSqliteString(source)}`];
  if (entry.scope === 'utid') {
    parts.push(`tc.utid = ${entry.id}`);
  } else {
    parts.push(`coalesce(tc.upid, t.upid) = ${entry.id}`);
  }
  if (typeof entry.session === 'number') {
    parts.push(`p.session_id = ${entry.session}`);
  } else if (entry.session === 'null') {
    parts.push('p.session_id is null');
  }
  return `(${parts.join(' and ')})`;
}

// OR-joins the conditions of the given context keys. Overlapping contexts
// (a thread and its process) never double count — the conditions select
// sample rows, and a row matching several conditions is still one row.
export function contextFilterForKeys(
  source: string,
  keys: ReadonlyArray<string>,
): string {
  return keys
    .map((key) => contextSqlCondition(source, parseContextKey(key)))
    .join(' or ');
}

// The counter names queryable for the given contexts: any 'all'-session
// context (or none scoped to a numeric session) exposes every counter of the
// source; otherwise only the counters of the named sessions.
export function counterNamesForKeys(
  keys: ReadonlyArray<string>,
  sourceCounterNames: readonly string[],
  counterNamesBySession: ReadonlyMap<number, readonly string[]>,
): string[] {
  const sessions = keys.map((key) => parseContextKey(key).session);
  const numeric = sessions.filter((s): s is number => typeof s === 'number');
  const hasAll = sessions.some((s) => s === 'all');
  const names =
    hasAll || numeric.length === 0
      ? sourceCounterNames
      : numeric.flatMap((s) => counterNamesBySession.get(s) ?? []);
  return [...new Set(names)];
}

// Matches an aggregated (utid, upid, session_id) sample bucket against a
// context, mirroring contextSqlCondition's semantics in TS. Used to fold
// per-bucket totals into per-context grid rows.
export function bucketMatchesContext(
  entry: ContextEntry,
  bucket: {
    readonly utid: number | null;
    readonly upid: number | null;
    readonly sessionId: number | null;
  },
): boolean {
  if (entry.scope === 'utid') {
    if (bucket.utid !== entry.id) return false;
  } else {
    if (bucket.upid !== entry.id) return false;
  }
  if (entry.session === 'all') return true;
  if (entry.session === 'null') return bucket.sessionId === null;
  return bucket.sessionId === entry.session;
}
