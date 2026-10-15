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

import {HttpError, InvalidResponseError} from './api';

const MIN_BACKOFF_MS = 1_000;
const MAX_BACKOFF_MS = 10_000;

/** Exponential backoff delays: 1 s, 2 s, 4 s, 8 s, 10 s, 10 s, ... */
export class Backoff {
  private delayMs = MIN_BACKOFF_MS;

  next(): number {
    const delayMs = this.delayMs;
    this.delayMs = Math.min(delayMs * 2, MAX_BACKOFF_MS);
    return delayMs;
  }

  reset(): void {
    this.delayMs = MIN_BACKOFF_MS;
  }
}

/**
 * Whether a failed request may succeed later: the server was unreachable or
 * unavailable, e.g. 503 while it has no data yet.
 */
export function isTransient(e: unknown): boolean {
  if (e instanceof HttpError) return e.status >= 500;
  return !(e instanceof InvalidResponseError);
}

/** Resolves after `ms`, or as soon as `signal` aborts. */
export function sleep(ms: number, signal: AbortSignal): Promise<void> {
  return new Promise((resolve) => {
    const done = () => {
      clearTimeout(timer);
      signal.removeEventListener('abort', done);
      resolve();
    };
    const timer = setTimeout(done, ms);
    signal.addEventListener('abort', done);
    if (signal.aborted) done();
  });
}
