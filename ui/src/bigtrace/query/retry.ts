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

// Retries for the value pickers' backend calls (the corpus filter chips and
// the grids' Equals filters).

import {BigtraceHttpError, QueryCancelledError} from './bigtrace_query_client';

// A value load that failed for a reason that may clear up on its own:
// backend load shedding (429/5xx) or a dropped connection (fetch rejects
// with a TypeError). Cancellations and 4xx answers are not; the latter won't
// change by asking again.
export function isTransientError(e: unknown): boolean {
  if (e instanceof QueryCancelledError) return false;
  if (e instanceof BigtraceHttpError) {
    return e.status === 429 || e.status >= 500;
  }
  return e instanceof TypeError;
}

// How many more times a transient failure is tried before it is shown, and
// the pause before the first of them (doubled each time, plus jitter so a
// burst of pickers doesn't knock on a shedding backend in lockstep).
export const RETRY_ATTEMPTS = 2;
export const RETRY_BASE_MS = 500;

export function retryDelayMs(attempt: number, random = Math.random): number {
  const base = RETRY_BASE_MS * 2 ** attempt;
  return base + random() * base * 0.5;
}

// Resolves after `ms`, or rejects with QueryCancelledError as soon as
// `signal` aborts, so a newer request doesn't wait out a backoff or a typing
// pause.
export function sleep(ms: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      reject(new QueryCancelledError());
      return;
    }
    const onAbort = () => {
      clearTimeout(timer);
      reject(new QueryCancelledError());
    };
    const timer = setTimeout(() => {
      signal?.removeEventListener('abort', onAbort);
      resolve();
    }, ms);
    signal?.addEventListener('abort', onAbort, {once: true});
  });
}

// Runs `attempt`, retrying transient failures up to RETRY_ATTEMPTS times
// with backoff. Anything else (or an abort) is thrown straight away. `wait`
// is injectable for tests.
export async function withRetry<T>(
  attempt: () => Promise<T>,
  signal?: AbortSignal,
  wait: (ms: number, signal?: AbortSignal) => Promise<void> = sleep,
): Promise<T> {
  for (let i = 0; ; i++) {
    try {
      return await attempt();
    } catch (e) {
      if (i >= RETRY_ATTEMPTS || !isTransientError(e)) throw e;
      await wait(retryDelayMs(i), signal);
    }
  }
}
