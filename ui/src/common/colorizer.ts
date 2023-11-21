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

import {hsl} from 'color-convert';
import {hsluvToRgb} from 'hsluv';

import {clamp} from '../base/math_utils';
import {hash} from '../common/hash';
import {cachedHsluvToHex} from '../frontend/hsluv_cache';

export interface Color {
  h: number;
  s: number;
  l: number;
  a?: number;
}

const MD_PALETTE: Color[] = [
  {h: 4, s: 90, l: 58},
  {h: 340, s: 82, l: 52},
  {h: 291, s: 64, l: 42},
  {h: 262, s: 52, l: 47},
  {h: 231, s: 48, l: 48},
  {h: 207, s: 90, l: 54},
  {h: 199, s: 98, l: 48},
  {h: 187, s: 100, l: 42},
  {h: 174, s: 100, l: 29},
  {h: 122, s: 39, l: 49},
  {h: 88, s: 50, l: 53},
  {h: 66, s: 70, l: 54},
  {h: 45, s: 100, l: 51},
  {h: 36, s: 100, l: 50},
  {h: 14, s: 100, l: 57},
  {h: 16, s: 25, l: 38},
  {h: 200, s: 18, l: 46},
  {h: 54, s: 100, l: 62},
];

export const GRAY_COLOR: Color = {
  h: 0,
  s: 0,
  l: 62,
};

// A piece of wisdom from a long forgotten blog post: "Don't make
// colors you want to change something normal like grey."
export const UNEXPECTED_PINK_COLOR: Color = {
  h: 330,
  s: 1.0,
  l: 0.706,
};

export function hueForCpu(cpu: number): number {
  return (128 + (32 * cpu)) % 256;
}

const DESAT_RED: Color = {
  h: 3,
  s: 30,
  l: 49,
};
const DARK_GREEN: Color = {
  h: 120,
  s: 44,
  l: 34,
};
const LIME_GREEN: Color = {
  h: 75,
  s: 55,
  l: 47,
};
const TRANSPARENT_WHITE: Color = {
  h: 0,
  s: 1,
  l: 97,
  a: 0.55,
};
const ORANGE: Color = {
  h: 36,
  s: 100,
  l: 50,
};
const INDIGO: Color = {
  h: 231,
  s: 48,
  l: 48,
};

export function colorForState(state: string): Readonly<Color> {
  if (state === 'Running') {
    return DARK_GREEN;
  } else if (state.startsWith('Runnable')) {
    return LIME_GREEN;
  } else if (state.includes('Uninterruptible Sleep')) {
    if (state.includes('non-IO')) {
      return DESAT_RED;
    }
    return ORANGE;
  } else if (state.includes('Sleeping') || state.includes('Idle')) {
    return TRANSPARENT_WHITE;
  }
  return INDIGO;
}

export function textColorForState(stateCode: string): string {
  const background = colorForState(stateCode);
  return background.l > 80 ? '#404040' : '#fff';
}

export function colorForString(identifier: string): Color {
  const colorIdx = hash(identifier, MD_PALETTE.length);
  return Object.assign({}, MD_PALETTE[colorIdx]);
}

export function colorForTid(tid: number): Color {
  return colorForString(tid.toString());
}

export function colorForThread(thread?: {pid?: number, tid: number}): Color {
  if (thread === undefined) {
    return Object.assign({}, GRAY_COLOR);
  }
  const tid = thread.pid ? thread.pid : thread.tid;
  return colorForTid(tid);
}

// 40 different random hues 9 degrees apart.
export function randomColor(): string {
  const hue = Math.floor(Math.random() * 40) * 9;
  return '#' + hsl.hex([hue, 90, 30]);
}

// Chooses a color uniform at random based on hash(sliceName).  Returns [hue,
// saturation, lightness].
//
// Prefer converting this to an RGB color using hsluv, not the browser's
// built-in vanilla HSL handling.  This is because this function chooses
// hue/lightness uniform at random, but HSL is not perceptually uniform.  See
// https://www.boronine.com/2012/03/26/Color-Spaces-for-Human-Beings/.
//
// If isSelected, the color will be particularly dark, making it stand out.
export function hslForSlice(
    sliceName: string, isSelected: boolean|null): [number, number, number] {
  const hue = hash(sliceName, 360);
  // Saturation 100 would give the most differentiation between colors, but it's
  // garish.
  const saturation = 80;
  const lightness = isSelected ? 30 : hash(sliceName + 'x', 40) + 40;
  return [hue, saturation, lightness];
}

// Lightens the color for thread slices to represent wall time.
export function colorForThreadIdleSlice(
    hue: number,
    saturation: number,
    lightness: number,
    isSelected: boolean|null): string {
  // Increase lightness by 80% when selected and 40% otherwise,
  // without exceeding 88.
  let newLightness = isSelected ? lightness * 1.8 : lightness * 1.4;
  newLightness = Math.min(newLightness, 88);
  return cachedHsluvToHex(hue, saturation, newLightness);
}

export function colorCompare(x: Color, y: Color): number {
  return (x.h - y.h) || (x.s - y.s) || (x.l - y.l);
}

// Return true if two colors have the same value.
export function colorsEqual(a: Color, b: Color): boolean {
  return a.h === b.h && a.s === b.s && a.l === b.l && a.a === b.a;
}

export function getColorForSlice(
    sliceName: string, hasFocus: boolean|null): Color {
  const name = sliceName.replace(/( )?\d+/g, '');
  const [hue, saturation, lightness] = hslForSlice(name, hasFocus);

  return {
    h: hue,
    s: saturation,
    l: lightness,
  };
}

const LIGHTNESS_MAX = 100;
const LIGHTNESS_MIN = 0;

// Lighten color by a percentage.
export function colorLighten(color: Color, amount: number): Color {
  return {
    ...color,
    l: clamp(color.l + amount, LIGHTNESS_MIN, LIGHTNESS_MAX),
  };
}

// Darken color by a percentage.
export function colorDarken(color: Color, amount: number): Color {
  return colorLighten(color, -amount);
}

const SATURATION_MAX = 100;
const SATURATION_MIN = 0;

// Saturate color by a percentage.
export function colorSaturate(color: Color, amount: number): Color {
  return {
    ...color,
    s: clamp(color.s + amount, SATURATION_MIN, SATURATION_MAX),
  };
}

// Desaturate color by a percentage.
export function colorDesaturate(color: Color, amount: number): Color {
  return colorSaturate(color, -amount);
}

// Convert color to RGB values in the range 0-255
export function colorToRGB(color: Color): [number, number, number] {
  const h = color.h;
  const s = color.s / SATURATION_MAX;
  const l = color.l / LIGHTNESS_MAX;

  const c = (1 - Math.abs(2 * l - 1)) * s;
  const x = c * (1 - Math.abs((h / 60) % 2 - 1));
  const m = l - c / 2;

  let [r, g, b] = [0, 0, 0];

  if (0 <= h && h < 60) {
    [r, g, b] = [c, x, 0];
  } else if (60 <= h && h < 120) {
    [r, g, b] = [x, c, 0];
  } else if (120 <= h && h < 180) {
    [r, g, b] = [0, c, x];
  } else if (180 <= h && h < 240) {
    [r, g, b] = [0, x, c];
  } else if (240 <= h && h < 300) {
    [r, g, b] = [x, 0, c];
  } else if (300 <= h && h < 360) {
    [r, g, b] = [c, 0, x];
  }

  // Convert to 0-255 range
  r = Math.round((r + m) * 255);
  g = Math.round((g + m) * 255);
  b = Math.round((b + m) * 255);

  return [r, g, b];
}

// Get whether a color should be considered "light" based on its perceived
// brightness.
export function colorIsLight(color: Color): boolean {
  // YIQ calculation from https://24ways.org/2010/calculating-color-contrast
  const [r, g, b] = hsluvToRgb([color.h, color.s, color.l]);
  const yiq = ((r * 299) + (g * 587) + (b * 114)) / 1000;
  return (yiq >= 128);
}

export function colorIsDark(color: Color): boolean {
  return !colorIsLight(color);
}
