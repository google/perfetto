// Copyright (C) 2019 The Android Open Source Project
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
  Color,
  colorCompare,
  colorForThread,
  colorIsLight,
  colorLighten,
  colorSaturate,
  colorsEqual,
  colorToRGB,
  hueForCpu,
} from './colorizer';

const PROCESS_A_THREAD_A = {
  tid: 100,
  pid: 100,
};

const PROCESS_A_THREAD_B = {
  tid: 101,
  pid: 100,
};

const PROCESS_B_THREAD_A = {
  tid: 200,
  pid: 200,
};

const PROCESS_UNK_THREAD_A = {
  tid: 42,
};

const PROCESS_UNK_THREAD_B = {
  tid: 42,
};

test('it gives threads colors by pid if present', () => {
  const colorAA = colorForThread(PROCESS_A_THREAD_A);
  const colorAB = colorForThread(PROCESS_A_THREAD_B);
  const colorBA = colorForThread(PROCESS_B_THREAD_A);
  expect(colorAA).toEqual(colorAB);
  expect(colorAA).not.toEqual(colorBA);
});

test('it gives threads colors by tid if pid missing', () => {
  const colorUnkA = colorForThread(PROCESS_UNK_THREAD_A);
  const colorUnkB = colorForThread(PROCESS_UNK_THREAD_B);
  expect(colorUnkA).toEqual(colorUnkB);
});

test('it copies colors', () => {
  const a = colorForThread(PROCESS_A_THREAD_A);
  const b = colorForThread(PROCESS_A_THREAD_A);
  expect(a === b).toEqual(false);
});

test('it gives different cpus different hues', () => {
  expect(hueForCpu(0)).not.toEqual(hueForCpu(1));
});

test('colorCompare', () => {
  const col: Color = {h: 123, s: 66, l: 45};

  expect(colorCompare({...col}, col)).toBe(0);

  expect(colorCompare({...col, h: 34}, col)).toBeLessThan(0);
  expect(colorCompare({...col, h: 156}, col)).toBeGreaterThan(0);

  expect(colorCompare({...col, s: 22}, col)).toBeLessThan(0);
  expect(colorCompare({...col, s: 100}, col)).toBeGreaterThan(0);

  expect(colorCompare({...col, l: 22}, col)).toBeLessThan(0);
  expect(colorCompare({...col, l: 76}, col)).toBeGreaterThan(0);
});

test('colorsEqual', () => {
  const col: Color = {h: 123, s: 66, l: 45};
  expect(colorsEqual(col, {h: 123, s: 66, l: 45})).toBeTruthy();
  expect(colorsEqual(col, {h: 86, s: 66, l: 45})).toBeFalsy();
  expect(colorsEqual(col, {h: 123, s: 43, l: 45})).toBeFalsy();
  expect(colorsEqual(col, {h: 123, s: 43, l: 78})).toBeFalsy();
});

test('colorLighten', () => {
  const col: Color = {h: 123, s: 66, l: 45};
  expect(colorLighten(col, 20)).toEqual({...col, l: 65});
  expect(colorLighten(col, 100)).toEqual({...col, l: 100});
  expect(colorLighten(col, -100)).toEqual({...col, l: 0});
});

test('colorSaturate', () => {
  const col: Color = {h: 123, s: 66, l: 45};
  expect(colorSaturate(col, 20)).toEqual({...col, s: 86});
  expect(colorSaturate(col, 100)).toEqual({...col, s: 100});
  expect(colorSaturate(col, -100)).toEqual({...col, s: 0});
});

test('colorToRGB', () => {
  // Pick a few well-known conversions to check we're in the right ballpark.
  expect(colorToRGB({h: 0, s: 0, l: 0})).toEqual([0, 0, 0]);
  expect(colorToRGB({h: 0, s: 100, l: 50})).toEqual([255, 0, 0]);
  expect(colorToRGB({h: 120, s: 100, l: 50})).toEqual([0, 255, 0]);
  expect(colorToRGB({h: 240, s: 100, l: 50})).toEqual([0, 0, 255]);
});

test('lightness calculations', () => {
  // Pick a few obvious light/dark colours to check we're in the right ballpark.
  expect(colorIsLight({h: 0, s: 0, l: 0})).toBeFalsy();
  expect(colorIsLight({h: 0, s: 0, l: 100})).toBeTruthy();

  expect(colorIsLight({h: 0, s: 0, l: 40})).toBeFalsy();
  expect(colorIsLight({h: 0, s: 0, l: 60})).toBeTruthy();
});
