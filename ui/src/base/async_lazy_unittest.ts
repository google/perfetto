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

import {AsyncLazy} from './async_lazy';
import {defer} from './deferred';
import {errResult, okResult, Result} from './result';

async function slowFactory(res: number): Promise<Result<number>> {
  const barrier = defer<void>();
  setTimeout(() => barrier.resolve(), 0);
  await barrier;
  return isFinite(res) ? okResult(res) : errResult(`${res} is not a number`);
}

test('AsyncLazy', async () => {
  const alazy = new AsyncLazy<number>();
  expect(alazy.value).toBeUndefined();

  // Failures during creation should not be cached.
  expect(await alazy.getOrCreate(() => slowFactory(NaN))).toEqual(
    errResult('NaN is not a number'),
  );
  expect(await alazy.getOrCreate(() => slowFactory(1 / 0))).toEqual(
    errResult('Infinity is not a number'),
  );

  const promises = [
    alazy.getOrCreate(() => slowFactory(42)),
    alazy.getOrCreate(() => slowFactory(1)),
    alazy.getOrCreate(() => slowFactory(2)),
  ];

  // Only the first promise will determine the result, which will be
  // subsequently cached.
  expect(await Promise.all(promises)).toEqual([
    okResult(42),
    okResult(42),
    okResult(42),
  ]);
  expect(alazy.value).toEqual(42);

  alazy.reset();
  expect(await alazy.getOrCreate(() => slowFactory(99))).toEqual(okResult(99));
});
