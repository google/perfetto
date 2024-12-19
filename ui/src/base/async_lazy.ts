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
import {okResult, Result} from './result';

/**
 * A utility class for lazily initializing and caching asynchronous values.
 *
 * This class ensures that a value is created only once using a provided
 * asynchronous factory function and is cached for future access. It also
 * provides methods to reset the cached value and retry the initialization.
 *
 * Internally, the class uses {@link AsyncGuard} to ensure non-overlapping of
 * the initialization process, preventing race conditions when multiple
 * callers attempt to initialize the value concurrently.
 */
export class AsyncLazy<T> {
  private _value?: T;
  private guard = new AsyncGuard<Result<T>>();

  getOrCreate(factory: () => Promise<Result<T>>): Promise<Result<T>> {
    if (this._value !== undefined) {
      return Promise.resolve(okResult(this._value));
    }

    const promise = this.guard.run(factory);
    promise.then((valueOrError) => {
      if (valueOrError.ok) {
        this._value = valueOrError.value;
      }
    });
    return promise;
  }

  get value(): T | undefined {
    return this._value;
  }

  reset() {
    this._value = undefined;
    this.guard = new AsyncGuard<Result<T>>();
  }
}
