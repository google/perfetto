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
  const k = CacheKey.create(201, 302, 123);
  const n = k.normalize();
  const n2 = n.normalize();
  expect(k.isNormalized()).toEqual(false);
  expect(n.isNormalized()).toEqual(true);
  expect(n2.isNormalized()).toEqual(true);
  expect(n).toEqual(n2);
  expect(n.startNs).toBeLessThanOrEqual(k.startNs);
  expect(n.endNs).toBeGreaterThanOrEqual(k.startNs);
  expect(n.bucketNs).toBeGreaterThanOrEqual(k.bucketNs);
  expect(Math.abs(n.windowSizePx - k.windowSizePx)).toBeLessThanOrEqual(200);
});

test('cache', () => {
  const k1 = (CacheKey.create(1000, 1100, 100)).normalize();
  const k2 = (CacheKey.create(2000, 2100, 100)).normalize();
  const k3 = (CacheKey.create(3000, 3100, 100)).normalize();
  const k4 = (CacheKey.create(4000, 4100, 100)).normalize();
  const k5 = (CacheKey.create(5000, 5100, 100)).normalize();
  const k6 = (CacheKey.create(6000, 6100, 100)).normalize();
  const k7 = (CacheKey.create(7000, 7100, 100)).normalize();
  const cache = new TrackCache<string>(5);

  cache.insert(k1, 'v1');
  expect(cache.lookup(k1)).toEqual('v1');

  cache.insert(k2, 'v2');
  cache.insert(k3, 'v3');
  cache.insert(k4, 'v4');
  cache.insert(k5, 'v5');

  // Should push k1/v1 out of the cache:
  cache.insert(k6, 'v6');
  expect(cache.lookup(k1)).toEqual(undefined);

  // Access k2 then add one more entry:
  expect(cache.lookup(k2)).toEqual('v2');
  cache.insert(k7, 'v7');

  // k2/v2 should still be present but k3/v3 should be discarded:
  expect(cache.lookup(k2)).toEqual('v2');
  expect(cache.lookup(k3)).toEqual(undefined);
});
