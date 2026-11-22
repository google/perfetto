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

import type {SnapshotData, TsValue} from '../sessions/live_session';
import {buildSystemTimeSeries} from './system_summary';

const MIB = 1024 * 1024;

function makeSnapshot(countersMib: Record<string, number>): SnapshotData {
  const systemCounters = new Map<string, TsValue[]>();
  const ts1 = 10_000_000_000;
  const ts2 = 11_000_000_000;
  for (const [name, mib] of Object.entries(countersMib)) {
    const bytes = mib * MIB;
    systemCounters.set(name, [
      {ts: ts1, value: bytes},
      {ts: ts2, value: bytes},
    ]);
  }
  return {
    systemCounters,
    processCountersByUpid: new Map(),
    processInfo: new Map(),
    lmkEvents: [],
    isUserDebug: true,
    ts0: ts1,
    xMin: 0,
    xMax: 1,
  };
}

describe('buildSystemTimeSeries', () => {
  test('accounts for >99.9% of MemTotal on Android /proc/meminfo including GPU, ION, Vmalloc, Misc, and Unevictable', () => {
    const snap = makeSnapshot({
      'MemTotal': 15552.1,
      'MemFree': 8452.2,
      'AnonPages': 2063.6,
      'Cached': 2493.3,
      'Shmem': 109.7,
      'Buffers': 0.7,
      'SwapCached': 0.3,
      'Active(anon)': 195.8,
      'Inactive(anon)': 1970.7,
      'Active(file)': 1698.1,
      'Inactive(file)': 381.4,
      'Unevictable': 311.7,
      'Slab': 601.4,
      'PageTables': 214.0,
      'KernelStack': 91.4,
      'VmallocUsed': 265.9,
      'Zram': 280.9,
      'ION_heap': 262.6,
      'ION_heap_pool': 144.2,
      'Gpu': 324.1,
      'Misc': 347.4,
    });

    const chart = buildSystemTimeSeries(snap, snap.ts0);
    expect(chart).toBeDefined();

    const byName = new Map(
      chart!.series.map((s) => [s.name, s.points[0]!.y / 1024]),
    );
    expect(byName.get('Anon')).toBeCloseTo(2063.6, 1);
    expect(byName.get('Page cache')).toBeCloseTo(2383.6, 1);
    expect(byName.get('DMA-BUF')).toBeCloseTo(406.8, 1);
    expect(byName.get('GPU')).toBeCloseTo(324.1, 1);
    expect(byName.get('VmallocUsed')).toBeCloseTo(265.9, 1);
    expect(byName.get('Kernel Misc')).toBeCloseTo(347.4, 1);
    expect(byName.get('Unaccounted')).toBeCloseTo(10.1, 1);
  });

  test('falls back to global GPU Memory counter and avoids double-counting DMA-BUF', () => {
    const snap = makeSnapshot({
      'MemTotal': 8192,
      'MemFree': 2000,
      'AnonPages': 2000,
      'Cached': 1500,
      'Shmem': 100,
      'Slab': 500,
      'ION_heap': 400,
      'ION_heap_pool': 100,
      'mem.dma_heap': 650,
      'GPU Memory': 800,
    });

    const chart = buildSystemTimeSeries(snap, snap.ts0);
    const byName = new Map(
      chart!.series.map((s) => [s.name, s.points[0]!.y / 1024]),
    );
    expect(byName.get('DMA-BUF')).toBeCloseTo(650, 1);
    expect(byName.get('GPU')).toBeCloseTo(800, 1);
    // 8192 - (2000 + 1400 + 100 + 500 + 650 + 800 + 2000) = 742
    expect(byName.get('Unaccounted')).toBeCloseTo(742, 1);
  });
});
