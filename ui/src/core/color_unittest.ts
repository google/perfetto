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

import {HSLColor, hslToRgb, HSLuvColor} from '../public/color';

describe('HSLColor', () => {
  const col = new HSLColor({h: 123, s: 66, l: 45});

  test('cssString', () => {
    expect(col.cssString).toBe('rgb(39 190 47)');
    expect(new HSLColor({h: 0, s: 0, l: 0}).cssString).toBe('rgb(0 0 0)');
    expect(new HSLColor({h: 0, s: 100, l: 100}).cssString).toBe(
      'rgb(255 255 255)',
    );
    expect(new HSLColor({h: 90, s: 25, l: 55}).cssString).toBe(
      'rgb(140 169 112)',
    );
    expect(new HSLColor({h: 180, s: 80, l: 40}, 0.7).cssString).toBe(
      'rgb(20 184 184 / 0.7)',
    );
  });

  test('lighten', () => {
    expect(col.lighten(20).hsl).toEqual([123, 66, 65]);
    expect(col.lighten(100).hsl).toEqual([123, 66, 100]);
    expect(col.lighten(-100).hsl).toEqual([123, 66, 0]);
  });

  test('saturate', () => {
    expect(col.saturate(20).hsl).toEqual([123, 86, 45]);
    expect(col.saturate(100).hsl).toEqual([123, 100, 45]);
    expect(col.saturate(-100).hsl).toEqual([123, 0, 45]);
  });

  test('setAlpha', () => {
    expect(col.setAlpha(0.7).alpha).toEqual(0.7);
    expect(col.setAlpha(undefined).alpha).toEqual(undefined);
  });

  test('perceivedBrightness', () => {
    // Test a few obviously light/dark colours.
    expect(new HSLColor({h: 0, s: 0, l: 0}).perceivedBrightness).toBeLessThan(
      128,
    );
    expect(
      new HSLColor({h: 0, s: 0, l: 100}).perceivedBrightness,
    ).toBeGreaterThan(128);

    expect(new HSLColor({h: 0, s: 0, l: 40}).perceivedBrightness).toBeLessThan(
      128,
    );
    expect(
      new HSLColor({h: 0, s: 0, l: 60}).perceivedBrightness,
    ).toBeGreaterThan(128);
  });
});

describe('HSLuvColor', () => {
  const col = new HSLuvColor({h: 123, s: 66, l: 45});

  test('cssString', () => {
    expect(col.cssString).toBe('rgb(69 117 58)');
    expect(new HSLColor({h: 0, s: 0, l: 0}).cssString).toBe('rgb(0 0 0)');
    expect(new HSLColor({h: 0, s: 100, l: 100}).cssString).toBe(
      'rgb(255 255 255)',
    );
    expect(new HSLuvColor({h: 90, s: 25, l: 55}).cssString).toBe(
      'rgb(131 133 112)',
    );
    expect(new HSLuvColor({h: 240, s: 100, l: 100}, 0.3).cssString).toBe(
      'rgb(254 255 255 / 0.3)',
    );
  });

  test('lighten', () => {
    expect(col.lighten(20).hsl).toEqual([123, 66, 65]);
    expect(col.lighten(100).hsl).toEqual([123, 66, 100]);
    expect(col.lighten(-100).hsl).toEqual([123, 66, 0]);
  });

  test('saturate', () => {
    expect(col.saturate(20).hsl).toEqual([123, 86, 45]);
    expect(col.saturate(100).hsl).toEqual([123, 100, 45]);
    expect(col.saturate(-100).hsl).toEqual([123, 0, 45]);
  });

  test('setAlpha', () => {
    expect(col.setAlpha(0.7).alpha).toEqual(0.7);
    expect(col.setAlpha(undefined).alpha).toEqual(undefined);
  });

  test('perceivedBrightness', () => {
    // Test a few obviously light/dark colours.
    expect(new HSLuvColor({h: 0, s: 0, l: 0}).perceivedBrightness).toBeLessThan(
      128,
    );
    expect(
      new HSLuvColor({h: 0, s: 0, l: 100}).perceivedBrightness,
    ).toBeGreaterThan(128);

    expect(
      new HSLuvColor({h: 0, s: 0, l: 40}).perceivedBrightness,
    ).toBeLessThan(128);
    expect(
      new HSLuvColor({h: 0, s: 0, l: 60}).perceivedBrightness,
    ).toBeGreaterThan(128);
  });
});

test('hslToRGB', () => {
  // Pick a few well-known conversions to check we're in the right ballpark.
  expect(hslToRgb(0, 0, 0)).toEqual([0, 0, 0]);
  expect(hslToRgb(0, 100, 50)).toEqual([255, 0, 0]);
  expect(hslToRgb(120, 100, 50)).toEqual([0, 255, 0]);
  expect(hslToRgb(240, 100, 50)).toEqual([0, 0, 255]);
});
