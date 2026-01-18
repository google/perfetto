// Copyright (C) 2025 The Android Open Source Project
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

// Generic cached resolver - caches results by dependency values.
// This class provides a type-safe way to compute and cache values based on
// specific properties of an input object.
export class CachedResolver<T, K extends keyof T, R> {
  // Use Map<unknown, R> to allow raw values as keys for single-key case
  private readonly cache = new Map<unknown, R>();

  constructor(
    private readonly keys: readonly K[],
    private readonly computeFn: (row: T) => R,
  ) {}

  get(row: T): R {
    // Fast path for single key (common case) - use raw value, no allocations
    const cacheKey =
      this.keys.length === 1
        ? row[this.keys[0]]
        : this.keys.map((k) => String(row[k] ?? '')).join(':');

    // NOTE: If compute returns undefined, we always re-call the function.
    // This is intentional - undefined means "try again" not "cache nothing".
    // We only cache non-undefined results.
    if (this.cache.has(cacheKey)) {
      return this.cache.get(cacheKey)!;
    }
    const result = this.computeFn(row);
    if (result !== undefined) {
      this.cache.set(cacheKey, result);
    }
    return result;
  }
}
