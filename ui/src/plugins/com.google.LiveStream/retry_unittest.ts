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
import {Backoff, isTransient, sleep} from './retry';

afterEach(() => {
  vi.useRealTimers();
});

test('Backoff doubles the delay up to 10 s', () => {
  const backoff = new Backoff();

  const delays = Array.from({length: 6}, () => backoff.next());
  expect(delays).toEqual([1000, 2000, 4000, 8000, 10000, 10000]);

  backoff.reset();
  expect(backoff.next()).toBe(1000);
});

test('isTransient', () => {
  expect(isTransient(new TypeError('Failed to fetch'))).toBe(true);
  expect(isTransient(new HttpError(503, 'no data yet'))).toBe(true);
  expect(isTransient(new HttpError(404, 'unknown target'))).toBe(false);
  expect(isTransient(new InvalidResponseError('bad JSON'))).toBe(false);
});

test('sleep resolves after the delay, or once aborted', async () => {
  vi.useFakeTimers();
  const done = vi.fn();
  void sleep(1000, new AbortController().signal).then(() => done('timer'));
  const controller = new AbortController();
  void sleep(10_000, controller.signal).then(() => done('abort'));

  await vi.advanceTimersByTimeAsync(1000);
  expect(done.mock.calls).toEqual([['timer']]);

  controller.abort();
  await vi.advanceTimersByTimeAsync(0);
  expect(done.mock.calls).toEqual([['timer'], ['abort']]);
});
