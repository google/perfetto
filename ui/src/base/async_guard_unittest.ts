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

import {AsyncGuard} from './async_guard';
import {defer} from './deferred';

test('AsyncGuard', async () => {
  const guard = new AsyncGuard<number>();
  let counter = 0;

  for (let i = 1; i <= 3; i++) {
    const barrier = defer<void>();
    const asyncTask = async () => {
      await barrier;
      return ++counter;
    };
    const promises = [
      guard.run(asyncTask),
      guard.run(asyncTask),
      guard.run(asyncTask),
    ];
    setTimeout(() => barrier.resolve(), 0);
    await barrier;
    expect(await Promise.all(promises)).toEqual([i, i, i]);
  }
});
