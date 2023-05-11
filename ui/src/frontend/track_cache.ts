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

import {assertTrue} from '../base/logging';

export const BUCKETS_PER_PIXEL = 2;

// CacheKey is a specific region of the timeline defined by the
// following four properties:
// - startNs
// - endNs
// - bucketNs
// - windowSizePx
// startNs is the beginning of the region in ns
// endNs is the end of the region in ns
// bucketNs is the size of a single bucket within the region which is
//          used for quantizing the timeline.
// windowSizePx is the size of the whole window in pixels.
//
// In the nominal case bucketNs is
// set so that 1px of the screen corresponds to N bucketNs worth of
// time where 1 < N < 10. This ensures that we show the maximum
// amount of data given the available screen real estate.
// We shouldn't rely on this property when rendering however since in
// some situations (i.e. after zooming before new data has loaded) it
// may not be the case.
//
// CacheKey's can be 'normalized' - rounding the interval up and the
// bucket size down. For a given CacheKey key ('foo') the normalized
// version ('normal') has the properties:
//   normal.startNs <= foo.startNs
//   normal.endNs => foo.endNs
//   normal.bucketNs <= foo.bucketNs
//   normal.windowSizePx ~= windowSizePx (we round to the nearest 100px)
//   foo.isCoveredBy(foo) == true
//   foo.isCoveredBy(normal) == true
//   normal.isCoveredBy(normal) == true
//   normal.isCoveredBy(foo) == false unless normal == foo
//   normalize(normal) == normal
//
// In other words the normal window is a superset of the data of the
// non-normal window at a higher resolution. Normalization is used to
// avoid re-fetching data on tiny zooms/moves/resizes.
// TODO(stevegolton): Convert to bigint timestamps.
export class CacheKey {
  readonly startNs: number;
  readonly endNs: number;
  readonly bucketNs: number;
  readonly windowSizePx: number;

  static create(startNs: number, endNs: number, windowSizePx: number):
      CacheKey {
    const bucketNs = (endNs - startNs) / (windowSizePx * BUCKETS_PER_PIXEL);
    return new CacheKey(startNs, endNs, bucketNs, windowSizePx);
  }

  private constructor(
      startNs: number, endNs: number, bucketNs: number, windowSizePx: number) {
    this.startNs = startNs;
    this.endNs = endNs;
    this.bucketNs = bucketNs;
    this.windowSizePx = windowSizePx;
  }

  static zero(): CacheKey {
    return new CacheKey(0, 0, 0, 100);
  }

  get normalizedBucketNs(): number {
    // Round bucketNs down to the nearest smaller power of 2 (minimum 1):
    return Math.max(1, Math.pow(2, Math.floor(Math.log2(this.bucketNs))));
  }

  get normalizedWindowSizePx(): number {
    return Math.max(100, Math.round(this.windowSizePx / 100) * 100);
  }

  normalize(): CacheKey {
    const windowSizePx = this.normalizedWindowSizePx;
    const bucketNs = this.normalizedBucketNs;
    const windowNs = windowSizePx * BUCKETS_PER_PIXEL * bucketNs;
    const startNs = Math.floor(this.startNs / windowNs) * windowNs;
    const endNs = Math.ceil(this.endNs / windowNs) * windowNs;
    return new CacheKey(startNs, endNs, bucketNs, windowSizePx);
  }

  isNormalized(): boolean {
    return this.toString() === this.normalize().toString();
  }

  isCoveredBy(other: CacheKey): boolean {
    let r = true;
    r = r && other.startNs <= this.startNs;
    r = r && other.endNs >= this.endNs;
    r = r && other.normalizedBucketNs === this.normalizedBucketNs;
    r = r && other.normalizedWindowSizePx === this.normalizedWindowSizePx;
    return r;
  }

  // toString is 'load bearing' in that it's used to key e.g. caches
  // with CacheKey's.
  toString() {
    const start = this.startNs;
    const end = this.endNs;
    const bucket = this.bucketNs;
    const size = this.windowSizePx;
    return `CacheKey<${start}, ${end}, ${bucket}, ${size}>`;
  }
}


interface CacheItem<T> {
  t: T;
  lastAccessId: number;
}


// LRU cache for the tracks.
// T is all the data needed for a displaying the track in a given
// CacheKey area - generally an array of slices.
export class TrackCache<T> {
  private cacheSize: number;
  private cache: Map<string, CacheItem<T>>;
  private lastAccessId: number;

  constructor(cacheSize: number) {
    assertTrue(cacheSize >= 2);
    this.cacheSize = cacheSize;
    this.cache = new Map();
    this.lastAccessId = 0;
  }

  insert(cacheKey: CacheKey, t: T): void {
    assertTrue(cacheKey.isNormalized());
    const key = cacheKey.toString();
    this.cache.set(key, {
      t,
      lastAccessId: this.lastAccessId++,
    });
    this.updateLru();
  }

  lookup(cacheKey: CacheKey): undefined|T {
    assertTrue(cacheKey.isNormalized());
    const key = cacheKey.toString();
    const item = this.cache.get(key);
    if (item) {
      item.lastAccessId = this.lastAccessId++;
      this.updateLru();
    }
    return item === undefined ? undefined : item.t;
  }

  private updateLru(): void {
    while (this.cache.size > this.cacheSize) {
      let oldestKey = '';
      let oldestAccessId = Number.MAX_SAFE_INTEGER;
      for (const [k, v] of this.cache.entries()) {
        if (v.lastAccessId < oldestAccessId) {
          oldestAccessId = v.lastAccessId;
          oldestKey = k;
        }
      }
      this.cache.delete(oldestKey);
    }
  }
}
