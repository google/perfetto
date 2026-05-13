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

import {formatDisplayValue} from './scorecard';

test('undefined value shows em-dash', () => {
  expect(formatDisplayValue(undefined)).toBe('\u2014');
});

test('string value is passed through', () => {
  expect(formatDisplayValue('4.2s')).toBe('4.2s');
  expect(formatDisplayValue('')).toBe('');
});

test('integer value is formatted with locale separators', () => {
  // toLocaleString is locale-dependent; just verify it returns a string
  const result = formatDisplayValue(1234);
  expect(result).toBeTruthy();
  expect(result).toContain('1');
});

test('decimal value is formatted with max 2 fraction digits', () => {
  const result = formatDisplayValue(3.14159);
  expect(result).toBeTruthy();
  // Should not have more than 2 fraction digits.
  // Use regex to handle both '.' and ',' as decimal separators.
  const match = result.match(/[.,](\d+)$/);
  if (match) {
    expect(match[1].length).toBeLessThanOrEqual(2);
  }
});

test('custom formatValue is applied to numbers', () => {
  const fmt = (v: number) => `${v}ms`;
  expect(formatDisplayValue(42, fmt)).toBe('42ms');
  expect(formatDisplayValue(0, fmt)).toBe('0ms');
});

test('custom formatValue is ignored for strings', () => {
  const fmt = (v: number) => `${v}ms`;
  expect(formatDisplayValue('already formatted', fmt)).toBe(
    'already formatted',
  );
});

test('custom formatValue is ignored for undefined', () => {
  const fmt = (v: number) => `${v}ms`;
  expect(formatDisplayValue(undefined, fmt)).toBe('\u2014');
});

test('zero value is displayed, not treated as falsy', () => {
  expect(formatDisplayValue(0)).toBe('0');
});

test('negative value is formatted correctly', () => {
  const result = formatDisplayValue(-42.5);
  expect(result).toContain('42');
});
