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

import {describe, expect, test} from 'vitest';

import type {AreaSelection} from '../../public/selection';
import type {Track} from '../../public/track';
import {
  bucketMatchesContext,
  contextEntriesForSelection,
  contextFilterForKeys,
  contextKey,
  counterNamesForKeys,
  parseContextKey,
  STACK_SAMPLE_TRACK_KIND,
} from './area_selection_contexts';

function track(tags: Record<string, unknown>): Track {
  return {
    uri: 'test',
    tags: {kinds: [STACK_SAMPLE_TRACK_KIND], ...tags},
  } as unknown as Track;
}

function selectionOf(tracks: Track[]): AreaSelection {
  return {tracks} as unknown as AreaSelection;
}

describe('contextEntriesForSelection', () => {
  test('extracts thread, process and session-scoped contexts', () => {
    const entries = contextEntriesForSelection(
      selectionOf([
        track({stackSampleSource: 'linux.perf', utid: 12}),
        track({stackSampleSource: 'linux.perf', upid: 4}),
        track({
          stackSampleSource: 'linux.perf',
          utid: 7,
          stackSampleSessionId: 3,
        }),
        track({
          stackSampleSource: 'linux.perf',
          utid: 8,
          stackSampleNullSession: true,
        }),
      ]),
      'linux.perf',
    );
    expect(entries).toEqual([
      {scope: 'utid', id: 12, session: 'all'},
      {scope: 'upid', id: 4, session: 'all'},
      {scope: 'utid', id: 7, session: 3},
      {scope: 'utid', id: 8, session: 'null'},
    ]);
  });

  test('skips other sources, other kinds and scopeless tracks', () => {
    const entries = contextEntriesForSelection(
      selectionOf([
        track({stackSampleSource: 'instruments', utid: 1}),
        track({stackSampleSource: 'linux.perf'}), // no utid/upid
        {uri: 'x', tags: {kinds: ['other'], utid: 2}} as unknown as Track,
      ]),
      'linux.perf',
    );
    expect(entries).toEqual([]);
  });

  test('dedupes contexts contributed by multiple tracks', () => {
    const entries = contextEntriesForSelection(
      selectionOf([
        track({stackSampleSource: 'linux.perf', utid: 12}),
        track({stackSampleSource: 'linux.perf', utid: 12}),
      ]),
      'linux.perf',
    );
    expect(entries).toHaveLength(1);
  });
});

describe('contextKey', () => {
  test('round-trips through parseContextKey', () => {
    const entries = [
      {scope: 'utid', id: 12, session: 'all'},
      {scope: 'upid', id: 4, session: 3},
      {scope: 'utid', id: 7, session: 'null'},
    ] as const;
    for (const entry of entries) {
      expect(parseContextKey(contextKey(entry))).toEqual(entry);
    }
  });

  test('formats human-scannable keys', () => {
    expect(contextKey({scope: 'utid', id: 12, session: 'all'})).toBe('utid=12');
    expect(contextKey({scope: 'upid', id: 4, session: 3})).toBe(
      'upid=4;session=3',
    );
    expect(contextKey({scope: 'utid', id: 7, session: 'null'})).toBe(
      'utid=7;session=null',
    );
  });
});

describe('contextFilterForKeys', () => {
  test('reproduces the per-context SQL fragments', () => {
    expect(
      contextFilterForKeys('linux.perf', ['utid=12', 'upid=4;session=3']),
    ).toBe(
      "(p.source = 'linux.perf' and tc.utid = 12) or " +
        "(p.source = 'linux.perf' and coalesce(tc.upid, t.upid) = 4 " +
        'and p.session_id = 3)',
    );
    expect(contextFilterForKeys('linux.perf', ['utid=7;session=null'])).toBe(
      "(p.source = 'linux.perf' and tc.utid = 7 and p.session_id is null)",
    );
  });
});

describe('counterNamesForKeys', () => {
  const bySession = new Map<number, readonly string[]>([
    [1, ['cpu-cycles']],
    [2, ['instructions']],
  ]);
  const all = ['cpu-cycles', 'instructions', 'cache-misses'];

  test('session-scoped contexts expose only their sessions counters', () => {
    expect(
      counterNamesForKeys(
        ['utid=1;session=1', 'utid=2;session=2'],
        all,
        bySession,
      ),
    ).toEqual(['cpu-cycles', 'instructions']);
  });

  test('an all-sessions context exposes every counter', () => {
    expect(
      counterNamesForKeys(['utid=1;session=1', 'upid=4'], all, bySession),
    ).toEqual(all);
  });

  test('null-session-only contexts fall back to every counter', () => {
    expect(
      counterNamesForKeys(['utid=7;session=null'], all, bySession),
    ).toEqual(all);
  });
});

describe('bucketMatchesContext', () => {
  test('matches thread and process buckets like the SQL conditions', () => {
    const threadCtx = {scope: 'utid', id: 12, session: 'all'} as const;
    expect(
      bucketMatchesContext(threadCtx, {utid: 12, upid: 4, sessionId: 1}),
    ).toBe(true);
    expect(
      bucketMatchesContext(threadCtx, {utid: 13, upid: 4, sessionId: 1}),
    ).toBe(false);

    const processCtx = {scope: 'upid', id: 4, session: 2} as const;
    expect(
      bucketMatchesContext(processCtx, {utid: 12, upid: 4, sessionId: 2}),
    ).toBe(true);
    expect(
      bucketMatchesContext(processCtx, {utid: 12, upid: 4, sessionId: 1}),
    ).toBe(false);
    expect(
      bucketMatchesContext(processCtx, {utid: null, upid: 5, sessionId: 2}),
    ).toBe(false);

    const nullSession = {scope: 'utid', id: 7, session: 'null'} as const;
    expect(
      bucketMatchesContext(nullSession, {utid: 7, upid: null, sessionId: null}),
    ).toBe(true);
    expect(
      bucketMatchesContext(nullSession, {utid: 7, upid: null, sessionId: 3}),
    ).toBe(false);
  });
});
