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
import {BigtraceHttpError, QueryCancelledError} from './bigtrace_query_client';
import {
  RETRY_ATTEMPTS,
  describeLoadError,
  isTransientError,
  retryDelayMs,
  withRetry,
} from './column_values_loader';

const RPC_503 =
  '{"error": {"code": 503, "message": "The service is currently ' +
  'unavailable.", "status": "UNAVAILABLE"}}';

describe('value loading retries', () => {
  test('isTransientError: load shedding and network drops, not 4xx/aborts', () => {
    expect(isTransientError(new BigtraceHttpError(503, RPC_503))).toBe(true);
    expect(isTransientError(new BigtraceHttpError(429, 'slow down'))).toBe(
      true,
    );
    expect(isTransientError(new BigtraceHttpError(500, 'boom'))).toBe(true);
    expect(isTransientError(new TypeError('Failed to fetch'))).toBe(true);
    expect(isTransientError(new BigtraceHttpError(400, 'bad'))).toBe(false);
    expect(isTransientError(new BigtraceHttpError(403, 'nope'))).toBe(false);
    expect(isTransientError(new QueryCancelledError())).toBe(false);
    expect(isTransientError(new Error('other'))).toBe(false);
  });

  test('withRetry retries transient failures with growing pauses', async () => {
    const waits: number[] = [];
    const wait = async (ms: number) => {
      waits.push(ms);
    };
    let calls = 0;
    const result = await withRetry(
      async () => {
        calls++;
        if (calls <= RETRY_ATTEMPTS) throw new BigtraceHttpError(503, RPC_503);
        return 'ok';
      },
      undefined,
      wait,
    );
    expect(result).toBe('ok');
    expect(calls).toBe(RETRY_ATTEMPTS + 1);
    expect(waits.length).toBe(RETRY_ATTEMPTS);
    expect(waits[1]).toBeGreaterThan(waits[0]);
  });

  test('withRetry gives up after the last attempt', async () => {
    let calls = 0;
    await expect(
      withRetry(
        async () => {
          calls++;
          throw new BigtraceHttpError(503, RPC_503);
        },
        undefined,
        async () => {},
      ),
    ).rejects.toBeInstanceOf(BigtraceHttpError);
    expect(calls).toBe(RETRY_ATTEMPTS + 1);
  });

  test('withRetry does not retry what will not change', async () => {
    let calls = 0;
    await expect(
      withRetry(
        async () => {
          calls++;
          throw new BigtraceHttpError(400, 'bad request');
        },
        undefined,
        async () => {},
      ),
    ).rejects.toBeInstanceOf(BigtraceHttpError);
    expect(calls).toBe(1);
  });

  test('withRetry stops as soon as the wait is aborted', async () => {
    let calls = 0;
    await expect(
      withRetry(
        async () => {
          calls++;
          throw new BigtraceHttpError(503, RPC_503);
        },
        undefined,
        async () => {
          throw new QueryCancelledError();
        },
      ),
    ).rejects.toBeInstanceOf(QueryCancelledError);
    expect(calls).toBe(1);
  });

  test('retryDelayMs doubles per attempt with bounded jitter', () => {
    expect(retryDelayMs(0, () => 0)).toBe(500);
    expect(retryDelayMs(1, () => 0)).toBe(1000);
    expect(retryDelayMs(0, () => 1)).toBe(750);
  });

  test('describeLoadError: backend message first, full text as detail', () => {
    const e = new BigtraceHttpError(503, RPC_503);
    const {summary, detail} = describeLoadError(e);
    expect(summary).toBe('The service is currently unavailable. (HTTP 503)');
    expect(detail).toBe(e.message);
  });

  test('describeLoadError: non-JSON body and network errors', () => {
    expect(
      describeLoadError(new BigtraceHttpError(502, 'Bad Gateway')),
    ).toEqual({
      summary: 'Bad Gateway (HTTP 502)',
      detail: 'HTTP 502: Bad Gateway',
    });
    expect(describeLoadError(new TypeError('Failed to fetch')).summary).toBe(
      'Could not reach the backend.',
    );
    expect(describeLoadError('weird')).toEqual({
      summary: 'weird',
      detail: 'weird',
    });
  });
});
