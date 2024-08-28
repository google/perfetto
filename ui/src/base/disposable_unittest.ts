// Copyright (C) 2023 The Android Open Source Project
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

import {AsyncDisposableStack, DisposableStack} from './disposable_stack';

test('DisposableStack', () => {
  const order: number[] = [];
  const trash = new DisposableStack();
  trash.use({[Symbol.dispose]: () => order.push(3)});
  trash.use({[Symbol.dispose]: () => order.push(2)});
  trash.defer(() => order.push(1));
  expect(order).toEqual([]);
  trash[Symbol.dispose]();
  expect(order).toEqual([1, 2, 3]);
});

test('AsyncDisposableStack', async () => {
  const order: number[] = [];
  const trash = new AsyncDisposableStack();
  trash.use({
    [Symbol.asyncDispose]: async () => {
      order.push(3);
    },
  });
  trash.use({
    [Symbol.asyncDispose]: async () => {
      order.push(2);
    },
  });
  trash.defer(async () => {
    order.push(1);
  });
  expect(order).toEqual([]);
  await trash[Symbol.asyncDispose]();
  expect(order).toEqual([1, 2, 3]);
});

test('AsyncDisposableStackWithDisposable', async () => {
  const trash = new AsyncDisposableStack();
  trash.use({
    [Symbol.dispose]: () => {
      console.log('Disposing...');
    },
  });
  await trash[Symbol.asyncDispose]();
});
