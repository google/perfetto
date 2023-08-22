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

import {hash} from '../common/hash';
import {cachedHsluvToHex} from '../frontend/hsluv_cache';

export interface Color {
  c: string;
  h: number;
  s: number;
  l: number;
  a?: number;
}

const MD_PALETTE: Color[] = [
  {c: 'red', h: 4, s: 90, l: 58},
  {c: 'pink', h: 340, s: 82, l: 52},
  {c: 'purple', h: 291, s: 64, l: 42},
  {c: 'deep purple', h: 262, s: 52, l: 47},
  {c: 'indigo', h: 231, s: 48, l: 48},
  {c: 'blue', h: 207, s: 90, l: 54},
  {c: 'light blue', h: 199, s: 98, l: 48},
  {c: 'cyan', h: 187, s: 100, l: 42},
  {c: 'teal', h: 174, s: 100, l: 29},
  {c: 'green', h: 122, s: 39, l: 49},
  {c: 'light green', h: 88, s: 50, l: 53},
  {c: 'lime', h: 66, s: 70, l: 54},
  {c: 'amber', h: 45, s: 100, l: 51},
  {c: 'orange', h: 36, s: 100, l: 50},
  {c: 'deep orange', h: 14, s: 100, l: 57},
  {c: 'brown', h: 16, s: 25, l: 38},
  {c: 'blue gray', h: 200, s: 18, l: 46},
  {c: 'yellow', h: 54, s: 100, l: 62},
];

export const GRAY_COLOR: Color = {
  c: 'grey',
  h: 0,
  s: 0,
  l: 62,
};

// A piece of wisdom from a long forgotten blog post: "Don't make
// colors you want to change something normal like grey."
export const UNEXPECTED_PINK_COLOR: Color = {
  c: '#ff69b4',
  h: 330,
  s: 1.0,
  l: 0.706,
};

export function hueForCpu(cpu: number): number {
  return (128 + (32 * cpu)) % 256;
}

const DESAT_RED: Color = {
  c: 'desat red',
  h: 3,
  s: 30,
  l: 49,
};
const DARK_GREEN: Color = {
  c: 'dark green',
  h: 120,
  s: 44,
  l: 34,
};
const LIME_GREEN: Color = {
  c: 'lime green',
  h: 75,
  s: 55,
  l: 47,
};
const TRANSPARENT_WHITE: Color = {
  c: 'white',
  h: 0,
  s: 1,
  l: 97,
  a: 0.55,
};
const ORANGE: Color = {
  c: 'orange',
  h: 36,
  s: 100,
  l: 50,
};
const INDIGO: Color = {
  c: 'indigo',
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

export function colorToStr(color: Color) {
  if (color.a !== undefined) {
    return `hsla(${color.h}, ${color.s}%, ${color.l}%, ${color.a})`;
  }
  return `hsl(${color.h}, ${color.s}%, ${color.l}%)`;
}

export function colorCompare(x: Color, y: Color) {
  return (x.h - y.h) || (x.s - y.s) || (x.l - y.l);
}

export function getColorForSlice(
    sliceName: string, hasFocus: boolean|null): Color {
  const name = sliceName.replace(/( )?\d+/g, '');
  const [hue, saturation, lightness] = hslForSlice(name, hasFocus);

  return {
    c: cachedHsluvToHex(hue, saturation, lightness),
    h: hue,
    s: saturation,
    l: lightness,
  };
}
