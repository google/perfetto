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

import {SharedAsyncDisposable} from './shared_disposable';

describe('SharedDisposableAsync', () => {
  it('allows access to the underlying disposable', async () => {
    const order: string[] = [];

    const disposable = {
      [Symbol.asyncDispose]: async () => {
        order.push('dispose');
      },
    };

    const shared = SharedAsyncDisposable.wrap(disposable);

    expect(shared.get()).toBe(disposable);
  });

  it('only disposes after refcount drops to 0', async () => {
    const order: string[] = [];

    const disposable = {
      [Symbol.asyncDispose]: async () => {
        order.push('dispose');
      },
    };

    order.push('create a');
    const a = SharedAsyncDisposable.wrap(disposable);

    order.push('clone b');
    const b = a.clone();

    order.push('dispose a');
    await a[Symbol.asyncDispose]();

    order.push('dispose b');
    await b[Symbol.asyncDispose]();

    expect(order).toEqual([
      'create a',
      'clone b',
      'dispose a',
      'dispose b',
      'dispose',
    ]);
  });

  it('throws on double dispose', async () => {
    const disposable = {
      [Symbol.asyncDispose]: async () => {},
    };

    const shared = SharedAsyncDisposable.wrap(disposable);
    await shared[Symbol.asyncDispose]();

    // Second dispose should fail
    await expect(shared[Symbol.asyncDispose]()).rejects.toThrow();
  });

  it('throws on clone after dispose', async () => {
    const disposable = {
      [Symbol.asyncDispose]: async () => {},
    };

    const shared = SharedAsyncDisposable.wrap(disposable);
    await shared[Symbol.asyncDispose]();

    // Clone after dispose should fail
    expect(() => shared.clone()).toThrow();
  });

  it('reveals isDisposed status', async () => {
    const disposable = {
      [Symbol.asyncDispose]: async () => {},
    };

    const shared = SharedAsyncDisposable.wrap(disposable);
    expect(shared.isDisposed).toBe(false);

    await shared[Symbol.asyncDispose]();
    expect(shared.isDisposed).toBe(true);
  });
});
