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

import {Time} from '../../base/time';
import {ProfileType} from './common';
import {
  flamegraphMetricsForHeapProfiles,
  heapProfileAllocMetricNames,
  heapProfileProcessTotalsSql,
} from './heap_profile_details_panel';

const TS = Time.fromRaw(100n);
const TS_END = 200n;

describe('heapProfileAllocMetricNames', () => {
  test('exposes the per-type measure sets', () => {
    expect(
      heapProfileAllocMetricNames(ProfileType.NATIVE_HEAP_PROFILE).map(
        (metric) => metric.name,
      ),
    ).toEqual([
      'Unreleased Malloc Size',
      'Unreleased Malloc Count',
      'Total Malloc Size',
      'Total Malloc Count',
    ]);
    expect(
      heapProfileAllocMetricNames(ProfileType.JAVA_HEAP_SAMPLES).map(
        (metric) => metric.name,
      ),
    ).toEqual(['Total Allocation Size', 'Total Allocation Count']);
  });

  test('rejects non-allocation profile types', () => {
    expect(() =>
      heapProfileAllocMetricNames(ProfileType.JAVA_HEAP_GRAPH),
    ).toThrow();
  });
});

describe('flamegraphMetricsForHeapProfiles', () => {
  test('bounds each process window by its own next dump', () => {
    const [metric] = flamegraphMetricsForHeapProfiles(
      TS,
      TS_END,
      [3, 7],
      'libc.malloc',
      heapProfileAllocMetricNames(ProfileType.NATIVE_HEAP_PROFILE),
    );
    const sql = metric.statement;
    // Per-upid right-extension of the window (min(ts) of the next dump).
    expect(sql).toContain('select upid, min(ts) as bound_ts');
    expect(sql).toContain('group by upid');
    expect(sql).toContain('upid in (3,7)');
    expect(sql).toContain("heap_name = 'libc.malloc'");
    expect(sql).toContain('ifnull(b.bound_ts, 200)');
    // Unreleased-classification must never merge across processes.
    expect(sql).toContain(
      'select upid, callsite_id, if(sum(count) > 0, 1, 0) as positive_alloc',
    );
    expect(sql).toContain('join alloc_class using (upid, callsite_id)');
  });

  test('single-upid metrics keep the historical shape', () => {
    const metrics = flamegraphMetricsForHeapProfiles(
      TS,
      TS_END,
      [42],
      'libc.malloc',
      heapProfileAllocMetricNames(ProfileType.NATIVE_HEAP_PROFILE),
    );
    expect(metrics.map((metric) => metric.name)).toEqual([
      'Unreleased Malloc Size',
      'Unreleased Malloc Count',
      'Total Malloc Size',
      'Total Malloc Count',
    ]);
    expect(metrics[0].unit).toBe('B');
    expect(metrics[0].statement).toContain('upid in (42)');
    expect(metrics[0].dependencySql).toContain(
      'android.memory.heap_profile.callstacks',
    );
  });

  test('escapes the heap name', () => {
    const [metric] = flamegraphMetricsForHeapProfiles(
      TS,
      TS_END,
      [1],
      "who'se heap",
      heapProfileAllocMetricNames(ProfileType.GENERIC_HEAP_PROFILE),
    );
    expect(metric.statement).toContain("'who''se heap'");
  });
});

describe('heapProfileProcessTotalsSql', () => {
  test('sums per process over the same bounded window', () => {
    const sql = heapProfileProcessTotalsSql(TS, TS_END, [3, 7], 'libc.malloc');
    expect(sql).toContain('select upid, min(ts) as bound_ts');
    expect(sql).toContain('count(distinct w.ts) as dumps');
    expect(sql).toContain('sum(max(w.size, 0)) as allocSize');
    expect(sql).toContain('sum(max(w.count, 0)) as allocCount');
    expect(sql).toContain('group by w.upid');
  });
});
