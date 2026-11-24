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

import {afterEach, beforeEach, describe, expect, test, vi} from 'vitest';
import m from 'mithril';
import {BigtraceHttpError, QueryCancelledError} from './bigtrace_query_client';
import {SEARCH_DEBOUNCE_MS} from './fetch_scheduler';
import {RETRY_BASE_MS} from './retry';
import {DistinctValues} from './distinct_values';

interface Call {
  readonly search: string;
  readonly signal: AbortSignal;
  resolve: (values: ReadonlyArray<string>) => void;
  reject: (e: unknown) => void;
}

// A DistinctValues whose requests the test answers by hand. An aborted
// request rejects the way the client does.
function harness() {
  const values = new DistinctValues();
  const calls: Call[] = [];
  const use = (search: string, version?: number) =>
    values.use(
      {column: 'name', search, scope: ''},
      (signal) =>
        new Promise<ReadonlyArray<string>>((resolve, reject) => {
          calls.push({search, signal, resolve, reject});
          signal.addEventListener('abort', () =>
            reject(new QueryCancelledError()),
          );
        }),
      version,
    );
  return {values, calls, use};
}

const flush = () => vi.advanceTimersByTimeAsync(0);

describe('DistinctValues', () => {
  beforeEach(() => {
    vi.useFakeTimers();
    vi.spyOn(m, 'redraw').mockImplementation(() => {});
  });
  afterEach(() => {
    vi.useRealTimers();
    vi.restoreAllMocks();
  });

  test('a newer search cancels the one it replaces; the old list shows meanwhile', async () => {
    const {calls, use} = harness();
    use('');
    calls[0]!.resolve(['alpha', 'beta']);
    await flush();
    expect(use('')).toEqual({data: ['alpha', 'beta'], isPending: false});

    use('a');
    await vi.advanceTimersByTimeAsync(SEARCH_DEBOUNCE_MS);
    expect(calls.map((c) => c.search)).toEqual(['', 'a']);

    expect(use('al')).toEqual({data: ['alpha', 'beta'], isPending: true});
    expect(calls[1]!.signal.aborted).toBe(true);

    await flush();
    await vi.advanceTimersByTimeAsync(SEARCH_DEBOUNCE_MS);
    calls[2]!.resolve(['alpha']);
    await flush();
    expect(use('al')).toEqual({data: ['alpha'], isPending: false});
  });

  test('a shorter search waits for its own answer', async () => {
    const {calls, use} = harness();
    use('al');
    await vi.advanceTimersByTimeAsync(SEARCH_DEBOUNCE_MS);
    calls[0]!.resolve(['alpha']);
    await flush();
    // The answer for 'al' would pass for the whole list of 'a' (or of an
    // empty search, on reopening the picker).
    expect(use('a')).toEqual({data: undefined, isPending: true});
  });

  test('a new version refetches without cancelling the reply on its way', async () => {
    const {calls, use} = harness();
    use('', 100);
    expect(use('', 200)).toEqual({data: undefined, isPending: true});
    expect(calls[0]!.signal.aborted).toBe(false);

    calls[0]!.resolve(['alpha']);
    await flush();
    expect(use('', 200)).toEqual({data: ['alpha'], isPending: true});

    calls[1]!.resolve(['alpha', 'beta']);
    await flush();
    expect(use('', 200)).toEqual({data: ['alpha', 'beta'], isPending: false});
  });

  test('a transient failure is retried; a lasting one gives an empty list', async () => {
    const {values, calls, use} = harness();
    use('');
    calls[0]!.reject(new BigtraceHttpError(503, 'busy'));
    await vi.advanceTimersByTimeAsync(RETRY_BASE_MS * 1.5);
    expect(calls).toHaveLength(2);

    calls[1]!.reject(new BigtraceHttpError(400, 'bad column'));
    await flush();
    expect(use('')).toEqual({data: [], isPending: false});
    expect(calls).toHaveLength(2);

    values.invalidate();
    await flush();
    use('');
    expect(calls).toHaveLength(3);
  });
});
