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

import {CacheKey, TrackCache} from './track_cache';

test('cacheKeys', () => {
  const k = CacheKey.create(201n, 302n, 123);
  const n = k.normalize();
  const n2 = n.normalize();
  expect(k.isNormalized()).toEqual(false);
  expect(n.isNormalized()).toEqual(true);
  expect(n2.isNormalized()).toEqual(true);
  expect(n).toEqual(n2);
  expect(n.start).toBeLessThanOrEqual(k.start);
  expect(n.end).toBeGreaterThanOrEqual(k.start);
  expect(n.bucketSize).toBeGreaterThanOrEqual(k.bucketSize);
  expect(Math.abs(n.windowSizePx - k.windowSizePx)).toBeLessThanOrEqual(200);
});

test('cache', () => {
  const key1 = (CacheKey.create(1000n, 1100n, 100)).normalize();
  const key2 = (CacheKey.create(2000n, 2100n, 100)).normalize();
  const key3 = (CacheKey.create(3000n, 3100n, 100)).normalize();
  const key4 = (CacheKey.create(4000n, 4100n, 100)).normalize();
  const key5 = (CacheKey.create(5000n, 5100n, 100)).normalize();
  const key6 = (CacheKey.create(6000n, 6100n, 100)).normalize();
  const key7 = (CacheKey.create(7000n, 7100n, 100)).normalize();
  const cache = new TrackCache<string>(5);

  cache.insert(key1, 'v1');
  expect(cache.lookup(key1)).toEqual('v1');

  cache.insert(key2, 'v2');
  cache.insert(key3, 'v3');
  cache.insert(key4, 'v4');
  cache.insert(key5, 'v5');

  // Should push key1/v1 out of the cache:
  cache.insert(key6, 'v6');
  expect(cache.lookup(key1)).toEqual(undefined);

  // Access key2 then add one more entry:
  expect(cache.lookup(key2)).toEqual('v2');
  cache.insert(key7, 'v7');

  // key2/v2 should still be present but key3/v3 should be discarded:
  expect(cache.lookup(key2)).toEqual('v2');
  expect(cache.lookup(key3)).toEqual(undefined);
});
