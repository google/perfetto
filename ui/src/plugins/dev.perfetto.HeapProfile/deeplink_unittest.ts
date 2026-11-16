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

import {ProfileType} from './common';
import {
  parseHeapProfileDeeplinkArgs,
  resolveFlamegraphMetricName,
} from './deeplink';

describe('deeplink', () => {
  test('resolves flamegraph metric names', () => {
    expect(
      resolveFlamegraphMetricName(
        ProfileType.NATIVE_HEAP_PROFILE,
        'total_size',
      ),
    ).toBe('Total Malloc Size');
    expect(
      resolveFlamegraphMetricName(
        ProfileType.NATIVE_HEAP_PROFILE,
        'unreleased_size',
      ),
    ).toBe('Unreleased Malloc Size');
    expect(
      resolveFlamegraphMetricName(ProfileType.JAVA_HEAP_SAMPLES, 'total_size'),
    ).toBe('Total Allocation Size');
    expect(
      resolveFlamegraphMetricName(
        ProfileType.JAVA_HEAP_SAMPLES,
        'unreleased_size',
      ),
    ).toBeUndefined();
  });

  test('parses plugin args with zod', () => {
    expect(parseHeapProfileDeeplinkArgs({})).toBeUndefined();
    expect(
      parseHeapProfileDeeplinkArgs({
        process: 'com.example.app',
        type: 'com.android.art',
        metric: 'total_size',
      }),
    ).toEqual({
      process: 'com.example.app',
      type: 'com.android.art',
      metric: 'total_size',
    });
    expect(
      parseHeapProfileDeeplinkArgs({
        type: 'libc.malloc',
      }),
    ).toEqual({
      type: 'libc.malloc',
    });
    expect(parseHeapProfileDeeplinkArgs({process: ''})).toBeUndefined();
    expect(parseHeapProfileDeeplinkArgs({metric: 'unknown'})).toBeUndefined();
    expect(parseHeapProfileDeeplinkArgs({process: 123})).toBeUndefined();
  });
});
