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

import {Time} from '../base/time';
import {CacheKey} from './timeline_cache';

test('cacheKeys', () => {
  const k = CacheKey.create(Time.fromRaw(201n), Time.fromRaw(302n), 123);
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
