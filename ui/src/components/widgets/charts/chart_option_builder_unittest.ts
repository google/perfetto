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
  collectYAxisLabels,
  formatLabel,
  generateNiceTicks,
} from './chart_option_builder';

describe('formatLabel', () => {
  test('returns string representation when no formatter', () => {
    expect(formatLabel(42, undefined)).toBe('42');
    expect(formatLabel('hello', undefined)).toBe('hello');
  });

  test('applies function formatter', () => {
    const fmt = (v: number | string) => `${v} ms`;
    expect(formatLabel(100, fmt)).toBe('100 ms');
  });

  test('applies function formatter to category string value', () => {
    const fmt = (v: number | string) =>
      typeof v === 'string' ? v.toUpperCase() : String(v);
    expect(formatLabel('abc', fmt)).toBe('ABC');
  });

  test('applies string formatter with {value} placeholder', () => {
    expect(formatLabel(100, '{value} kg')).toBe('100 kg');
    expect(formatLabel('A', 'Category: {value}')).toBe('Category: A');
  });

  test('handles string formatter without placeholder', () => {
    expect(formatLabel(42, 'fixed')).toBe('fixed');
  });
});

describe('generateNiceTicks', () => {
  test('returns single value for zero range', () => {
    expect(generateNiceTicks(5, 5, false)).toEqual([5]);
  });

  test('generates ticks for 0-100 range', () => {
    const ticks = generateNiceTicks(0, 100, false);
    expect(ticks.length).toBeGreaterThanOrEqual(3);
    expect(ticks[0]).toBeLessThanOrEqual(0);
    expect(ticks[ticks.length - 1]).toBeGreaterThanOrEqual(100);
  });

  test('generates ticks for non-zero-based range', () => {
    const ticks = generateNiceTicks(100, 200, false);
    // Should NOT include 0 since data starts at 100.
    expect(ticks[0]).toBeGreaterThanOrEqual(100);
    expect(ticks[ticks.length - 1]).toBeGreaterThanOrEqual(200);
  });

  test('generates ticks for negative range', () => {
    const ticks = generateNiceTicks(-50, 50, false);
    expect(ticks[0]).toBeLessThanOrEqual(-50);
    expect(ticks[ticks.length - 1]).toBeGreaterThanOrEqual(50);
  });

  test('generates log-scale ticks', () => {
    const ticks = generateNiceTicks(1, 10000, true);
    expect(ticks).toEqual([1, 10, 100, 1000, 10000]);
  });

  test('log-scale with small values clamps to 1', () => {
    const ticks = generateNiceTicks(0.5, 100, true);
    expect(ticks[0]).toBe(1);
    expect(ticks[ticks.length - 1]).toBeGreaterThanOrEqual(100);
  });
});

describe('collectYAxisLabels', () => {
  test('returns empty for array yAxis (dual-axis)', () => {
    const opt = {
      yAxis: [{type: 'value'}, {type: 'value'}],
      series: [{data: [1, 2, 3]}],
    };
    expect(collectYAxisLabels(opt)).toEqual([]);
  });

  test('returns category data directly', () => {
    const opt = {
      yAxis: {type: 'category', data: ['A', 'B', 'C']},
      series: [],
    };
    expect(collectYAxisLabels(opt)).toEqual(['A', 'B', 'C']);
  });

  test('returns empty for category with no data', () => {
    const opt = {yAxis: {type: 'category'}, series: []};
    expect(collectYAxisLabels(opt)).toEqual([]);
  });

  test('extracts min/max from numeric series data', () => {
    const opt = {
      yAxis: {type: 'value'},
      series: [{data: [10, 20, 30]}],
    };
    const labels = collectYAxisLabels(opt);
    expect(labels.length).toBeGreaterThanOrEqual(2);
    expect(labels[0]).toBeLessThanOrEqual(10);
    expect(labels[labels.length - 1]).toBeGreaterThanOrEqual(30);
  });

  test('handles [x, y] pair data', () => {
    const opt = {
      yAxis: {type: 'value'},
      series: [
        {
          data: [
            [0, 5],
            [1, 15],
            [2, 25],
          ],
        },
      ],
    };
    const labels = collectYAxisLabels(opt);
    expect(labels.length).toBeGreaterThanOrEqual(2);
  });

  test('handles boxplot data [min, Q1, median, Q3, max]', () => {
    const opt = {
      yAxis: {type: 'value'},
      series: [{data: [[10, 20, 30, 40, 500]]}],
    };
    const labels = collectYAxisLabels(opt);
    // Should include ticks covering up to 500, not just Q1 (20).
    expect(labels[labels.length - 1]).toBeGreaterThanOrEqual(500);
  });

  test('handles {value: n} object data', () => {
    const opt = {
      yAxis: {type: 'value'},
      series: [{data: [{value: 100}, {value: 200}]}],
    };
    const labels = collectYAxisLabels(opt);
    expect(labels.length).toBeGreaterThanOrEqual(2);
    expect(labels[labels.length - 1]).toBeGreaterThanOrEqual(200);
  });

  test('returns empty when no series data', () => {
    const opt = {yAxis: {type: 'value'}, series: []};
    expect(collectYAxisLabels(opt)).toEqual([]);
  });

  test('returns empty when series has no data arrays', () => {
    const opt = {yAxis: {type: 'value'}, series: [{type: 'bar'}]};
    expect(collectYAxisLabels(opt)).toEqual([]);
  });
});
