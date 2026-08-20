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

import {describe, expect, test} from 'vitest';

import {aggregateProfileMetric, displayUnit} from './metrics';

describe('displayUnit', () => {
  test('maps pprof units onto the flamegraph unit vocabulary', () => {
    expect(displayUnit('nanoseconds')).toBe('ns');
    expect(displayUnit('Nanoseconds')).toBe('ns');
    expect(displayUnit('bytes')).toBe('B');
    expect(displayUnit('count')).toBe('count');
    expect(displayUnit('')).toBe('');
  });
});

describe('aggregateProfileMetric', () => {
  test('sums the given aggregate profiles in one statement', () => {
    const metric = aggregateProfileMetric(
      'cpu (nanoseconds)',
      'nanoseconds',
      [3, 7, 42],
    );
    expect(metric.name).toBe('cpu (nanoseconds)');
    expect(metric.unit).toBe('ns');
    expect(metric.nameColumnLabel).toBe('Symbol');
    expect(metric.dependencySql).toContain('callstacks.stack_profile');
    expect(metric.statement).toContain(
      'WHERE sample.aggregate_profile_id IN (3,7,42)',
    );
    expect(metric.statement).toContain(
      '_callstacks_for_stack_profile_samples!(profile_samples)',
    );
  });

  test('exposes mapping/source properties and the inlined marker', () => {
    const metric = aggregateProfileMetric(
      'cpu (nanoseconds)',
      'nanoseconds',
      [1],
    );
    expect(metric.unaggregatableProperties?.map((p) => p.name)).toEqual([
      'mapping_name',
      'inlined',
    ]);
    expect(metric.aggregatableProperties?.map((p) => p.name)).toEqual([
      'source_location',
    ]);
    const marker = metric.optionalMarker;
    expect(marker?.name).toBe('Inlined Function');
    expect(marker?.isVisible(new Map([['inlined', '1']]))).toBe(true);
    expect(marker?.isVisible(new Map([['inlined', '0']]))).toBe(false);
  });
});
