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

import {describe, expect, test, vi} from 'vitest';
import {BigtraceAsyncDataSource} from './bigtrace_async_data_source';
import {
  type BigtraceQueryClient,
  BigtraceHttpError,
  QueryCancelledError,
} from './bigtrace_query_client';

// A client whose fetchResults always rejects with `err`.
function rejectingClient(err: Error): BigtraceQueryClient {
  return {
    fetchResults: vi.fn().mockRejectedValue(err),
  } as unknown as BigtraceQueryClient;
}

describe('BigtraceAsyncDataSource error handling', () => {
  test('an HTTP error surfaces the backend detail and status', async () => {
    const client = rejectingClient(
      new BigtraceHttpError(400, "unknown column 'foo'"),
    );
    const ds = new BigtraceAsyncDataSource('uid', client, () => 0);
    await ds.ensureResultsLoaded();
    // The view shows the detail, not the wrapped `HTTP 400: ...` message.
    expect(ds.getError()).toBe("unknown column 'foo'");
    expect(ds.getErrorStatus()).toBe(400);
  });

  test('a non-HTTP error surfaces its message with no status', async () => {
    const client = rejectingClient(new Error('network down'));
    const ds = new BigtraceAsyncDataSource('uid', client, () => 0);
    await ds.ensureResultsLoaded();
    expect(ds.getError()).toBe('network down');
    expect(ds.getErrorStatus()).toBeUndefined();
  });

  test('a cancelled fetch is swallowed (no error surfaced)', async () => {
    const client = rejectingClient(new QueryCancelledError());
    const ds = new BigtraceAsyncDataSource('uid', client, () => 0);
    await ds.ensureResultsLoaded();
    expect(ds.getError()).toBeNull();
    expect(ds.getErrorStatus()).toBeUndefined();
  });
});
