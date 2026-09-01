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

/**
 * Memo: Synchronous memoization keyed by a JSON-serializable value.
 *
 * ## Why it exists
 *
 * Components often need to compute derived values that are expensive to
 * calculate but stable across render cycles when inputs haven't changed.
 * Memo provides a single-entry cache keyed by a JSON-serializable object:
 * - Call `use()` every render cycle with the current key and compute function
 * - Get back the cached value if the key hasn't changed
 * - Recompute automatically when the key changes
 *
 * ## Usage
 *
 * ```typescript
 * class MyPanel implements m.ClassComponent<Attrs> {
 *   private readonly computedCache = new Memo<MyData>();
 *
 *   view({attrs}: m.CVnode<Attrs>) {
 *     const data = this.computedCache.use({
 *       key: {filters: attrs.filters, count: this.count},
 *       compute: () => expensiveCompute(attrs.filters, this.count),
 *     });
 *
 *     return m('div', renderData(data));
 *   }
 * }
 * ```
 *
 * ## Key concepts
 *
 * - **key**: JSON-serializable object identifying the computation. Changes
 *   trigger a recompute. Supports primitives, arrays, objects, and bigints.
 * - **compute**: Synchronous function that returns the value to cache.
 *   Called only when the key changes.
 * - **disposal**: Cached `Disposable` values are disposed when replaced,
 *   invalidated, or when the memo itself is disposed.
 */

import {isDisposable} from './disposable';
import {type JSONCompatible, stringifyJsonWithBigints} from './json_utils';

export interface MemoOptions<T, K extends JSONCompatible<K>> {
  /**
   * JSON-serializable key identifying this computation.
   * Changes to the key (compared via stringifyJsonWithBigints)
   * trigger a recompute.
   */
  readonly key: K;
  /**
   * Synchronous function that computes the value.
   * Called only when the key changes.
   */
  readonly compute: () => T;
}

interface Cache<T> {
  readonly keyStr: string;
  readonly value: T;
}

/**
 * A single-entry synchronous memo cache.
 *
 * Created once per computed value on a component. Calls to `use()` with
 * the same key return the cached result; key changes trigger a recompute.
 * The memo owns cached `Disposable` values and disposes them when evicted.
 */
export class Memo<T> implements Disposable {
  private cache?: Cache<T>;

  use<K extends JSONCompatible<K>>(options: MemoOptions<T, K>): T {
    const {key, compute} = options;
    const keyStr = stringifyJsonWithBigints(key);

    if (this.cache !== undefined && this.cache.keyStr === keyStr) {
      return this.cache.value;
    }

    this.disposeCache();
    const value = compute();
    this.cache = {keyStr, value};
    return value;
  }

  /**
   * Clear and dispose the cached value, forcing the next use() to recompute.
   */
  invalidate(): void {
    this.disposeCache();
  }

  /** Dispose the cached value. */
  dispose(): void {
    this[Symbol.dispose]();
  }

  [Symbol.dispose](): void {
    this.disposeCache();
  }

  private disposeCache(): void {
    const cache = this.cache;
    this.cache = undefined;
    if (cache !== undefined && isDisposable(cache.value)) {
      cache.value[Symbol.dispose]();
    }
  }
}
