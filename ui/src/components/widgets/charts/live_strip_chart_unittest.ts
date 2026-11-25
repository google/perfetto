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

import {
  computeStripHoverInfo,
  sampleAt,
  stepLivePlayhead,
} from './live_strip_chart';

describe('LiveStripChart helpers', () => {
  test('stepLivePlayhead glides at constant 1.0x inside the jitter band', () => {
    const dt = 1 / 60;
    let cur = 100.0;
    const target = 102.0; // lead = 2.0s (inside [0.45, 2.35])
    for (let i = 0; i < 60; i++) {
      cur = stepLivePlayhead(cur, target, dt, 2.0);
    }
    expect(cur).toBeCloseTo(101.0, 4);
  });

  test('stepLivePlayhead gently catches up when lead exceeds lag + hysteresis', () => {
    const next = stepLivePlayhead(100.0, 104.0, 0.1, 2.0);
    // Lead = 4.0s > 2.35s -> rate > 1.0 -> advances > 0.1s
    expect(next - 100.0).toBeGreaterThan(0.12);
    expect(next).toBeLessThanOrEqual(104.0);
  });

  test('stepLivePlayhead feathers smoothly when approaching target and never overshoots', () => {
    const slow = stepLivePlayhead(101.8, 102.0, 0.05, 2.0);
    expect(slow - 101.8).toBeGreaterThan(0);
    expect(slow - 101.8).toBeLessThan(0.05);
    const clamped = stepLivePlayhead(101.995, 102.0, 0.1, 2.0);
    expect(clamped).toBeLessThanOrEqual(102.0);
  });

  test('sampleAt linearly interpolates between adjacent samples and avoids huge gaps', () => {
    const pts = [
      {t: 10, v: 20},
      {t: 12, v: 40},
      {t: 30, v: 100},
    ];
    expect(sampleAt(pts, 11)).toBeCloseTo(30, 5);
    expect(sampleAt(pts, 10)).toBe(20);
    expect(sampleAt(pts, 5)).toBeUndefined();
    // Large gap (> 6s) snaps to nearest sample rather than drawing false ramp.
    expect(sampleAt(pts, 14)).toBe(40);
    expect(sampleAt(pts, 28)).toBe(100);
  });

  test('computeStripHoverInfo updates live right-edge values as nowSec advances', () => {
    const series = [
      {
        name: 'Anon',
        color: '#7c3aed',
        pts: [
          {t: 100, v: 100},
          {t: 102, v: 200},
          {t: 104, v: 300},
        ],
      },
      {
        name: 'Free',
        color: '#16a34a',
        pts: [
          {t: 100, v: 400},
          {t: 102, v: 350},
          {t: 104, v: 250},
        ],
      },
    ];
    const at101 = computeStripHoverInfo({
      series,
      hoverX: 456,
      plotW: 400,
      nowSec: 101,
      winSec: 60,
      stacked: true,
      atRightEdge: true,
      formatY: (v) => `${Math.round(v)} MiB`,
    });
    const at103 = computeStripHoverInfo({
      series,
      hoverX: 456,
      plotW: 400,
      nowSec: 103,
      winSec: 60,
      stacked: true,
      atRightEdge: true,
      formatY: (v) => `${Math.round(v)} MiB`,
    });
    expect(at101?.timeLabel).toBe('Now');
    expect(at103?.timeLabel).toBe('Now');
    // Stacked tooltips list top band first and include a Total row.
    expect(at101?.items.map((i) => `${i.name}:${i.value}`)).toEqual([
      'Free:375 MiB',
      'Anon:150 MiB',
    ]);
    expect(at101?.totalValue).toBe('525 MiB');
    expect(at103?.items.map((i) => `${i.name}:${i.value}`)).toEqual([
      'Free:300 MiB',
      'Anon:250 MiB',
    ]);
    expect(at103?.totalValue).toBe('550 MiB');
    expect(at103?.signature).not.toBe(at101?.signature);
  });
});
