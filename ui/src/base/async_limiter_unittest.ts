// Copyright (C) 2024 The Android Open Source Project
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

import {AsyncLimiter} from './async_limiter';

test('no concurrent callbacks', async () => {
  const limiter = new AsyncLimiter();

  const mock1 = jest.fn();
  limiter.schedule(async () => mock1());
  expect(mock1).toHaveBeenCalled();

  const mock2 = jest.fn();
  limiter.schedule(async () => mock2());
  expect(mock2).not.toHaveBeenCalled();
});

test('queueing', async () => {
  const limiter = new AsyncLimiter();

  const mock1 = jest.fn();
  limiter.schedule(async () => mock1());

  const mock2 = jest.fn();
  await limiter.schedule(async () => mock2());

  expect(mock1).toHaveBeenCalled();
  expect(mock2).toHaveBeenCalled();
});

test('multiple queuing', async () => {
  const limiter = new AsyncLimiter();

  const mock1 = jest.fn();
  limiter.schedule(async () => mock1());

  const mock2 = jest.fn();
  limiter.schedule(async () => mock2());

  const mock3 = jest.fn();
  await limiter.schedule(async () => mock3());

  expect(mock1).toHaveBeenCalled();
  expect(mock2).not.toHaveBeenCalled();
  expect(mock3).toHaveBeenCalled();
});

test('error in callback bubbles up to caller', async () => {
  const limiter = new AsyncLimiter();
  const failingCallback = async () => {
    throw Error();
  };

  expect(async () => await limiter.schedule(failingCallback)).rejects.toThrow();
});

test('chain continues even when one callback fails', async () => {
  const limiter = new AsyncLimiter();

  const failingCallback = async () => {
    throw Error();
  };
  limiter.schedule(failingCallback).catch(() => {});

  const mock = jest.fn();
  await limiter.schedule(async () => mock());

  expect(mock).toHaveBeenCalled();
});
