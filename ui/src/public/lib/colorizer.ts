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
import {hash} from '../../base/hash';
import {featureFlags} from '../../core/feature_flags';
import {Color, HSLColor, HSLuvColor} from '../color';
import {ColorScheme} from '../color_scheme';
import {RandState, pseudoRand} from '../../base/rand';

// 128 would provide equal weighting between dark and light text.
// However, we want to prefer light text for stylistic reasons.
// A higher value means color must be brighter before switching to dark text.
const PERCEIVED_BRIGHTNESS_LIMIT = 180;

// This file defines some opinionated colors and provides functions to access
// random but predictable colors based on a seed, as well as standardized ways
// to access colors for core objects such as slices and thread states.

// We have, over the years, accumulated a number of different color palettes
// which are used for different parts of the UI.
// It would be nice to combine these into a single palette in the future, but
// changing colors is difficult especially for slice colors, as folks get used
// to certain slices being certain colors and are resistant to change.
// However we do it, we should make it possible for folks to switch back the a
// previous palette, or define their own.

const USE_CONSISTENT_COLORS = featureFlags.register({
  id: 'useConsistentColors',
  name: 'Use common color palette for timeline elements',
  description: 'Use the same color palette for all timeline elements.',
  defaultValue: false,
});

const randColourState: RandState = {seed: 0};

const MD_PALETTE_RAW: Color[] = [
  new HSLColor({h: 4, s: 90, l: 58}),
  new HSLColor({h: 340, s: 82, l: 52}),
  new HSLColor({h: 291, s: 64, l: 42}),
  new HSLColor({h: 262, s: 52, l: 47}),
  new HSLColor({h: 231, s: 48, l: 48}),
  new HSLColor({h: 207, s: 90, l: 54}),
  new HSLColor({h: 199, s: 98, l: 48}),
  new HSLColor({h: 187, s: 100, l: 42}),
  new HSLColor({h: 174, s: 100, l: 29}),
  new HSLColor({h: 122, s: 39, l: 49}),
  new HSLColor({h: 88, s: 50, l: 53}),
  new HSLColor({h: 66, s: 70, l: 54}),
  new HSLColor({h: 45, s: 100, l: 51}),
  new HSLColor({h: 36, s: 100, l: 50}),
  new HSLColor({h: 14, s: 100, l: 57}),
  new HSLColor({h: 16, s: 25, l: 38}),
  new HSLColor({h: 200, s: 18, l: 46}),
  new HSLColor({h: 54, s: 100, l: 62}),
];

const WHITE_COLOR = new HSLColor([0, 0, 100]);
const BLACK_COLOR = new HSLColor([0, 0, 0]);
const GRAY_COLOR = new HSLColor([0, 0, 90]);

const MD_PALETTE: ColorScheme[] = MD_PALETTE_RAW.map((color): ColorScheme => {
  const base = color.lighten(10, 60).desaturate(20);
  const variant = base.lighten(30, 80).desaturate(20);

  return {
    base,
    variant,
    disabled: GRAY_COLOR,
    textBase: WHITE_COLOR, // White text suits MD colors quite well
    textVariant: WHITE_COLOR,
    textDisabled: WHITE_COLOR, // Low contrast is on purpose
  };
});

// Create a color scheme based on a single color, which defines the variant
// color as a slightly darker and more saturated version of the base color.
export function makeColorScheme(base: Color, variant?: Color): ColorScheme {
  variant = variant ?? base.darken(15).saturate(15);

  return {
    base,
    variant,
    disabled: GRAY_COLOR,
    textBase:
      base.perceivedBrightness >= PERCEIVED_BRIGHTNESS_LIMIT
        ? BLACK_COLOR
        : WHITE_COLOR,
    textVariant:
      variant.perceivedBrightness >= PERCEIVED_BRIGHTNESS_LIMIT
        ? BLACK_COLOR
        : WHITE_COLOR,
    textDisabled: WHITE_COLOR, // Low contrast is on purpose
  };
}

const GRAY = makeColorScheme(new HSLColor([0, 0, 62]));
const DESAT_RED = makeColorScheme(new HSLColor([3, 30, 49]));
const DARK_GREEN = makeColorScheme(new HSLColor([120, 44, 34]));
const LIME_GREEN = makeColorScheme(new HSLColor([75, 55, 47]));
const TRANSPARENT_WHITE = makeColorScheme(new HSLColor([0, 1, 97], 0.55));
const ORANGE = makeColorScheme(new HSLColor([36, 100, 50]));
const INDIGO = makeColorScheme(new HSLColor([231, 48, 48]));

// A piece of wisdom from a long forgotten blog post: "Don't make
// colors you want to change something normal like grey."
export const UNEXPECTED_PINK = makeColorScheme(new HSLColor([330, 100, 70]));

// Selects a predictable color scheme from a palette of material design colors,
// based on a string seed.
function materialColorScheme(seed: string): ColorScheme {
  const colorIdx = hash(seed, MD_PALETTE.length);
  return MD_PALETTE[colorIdx];
}

const proceduralColorCache = new Map<string, ColorScheme>();

// Procedurally generates a predictable color scheme based on a string seed.
function proceduralColorScheme(seed: string): ColorScheme {
  const colorScheme = proceduralColorCache.get(seed);
  if (colorScheme) {
    return colorScheme;
  } else {
    const hue = hash(seed, 360);
    // Saturation 100 would give the most differentiation between colors, but
    // it's garish.
    const saturation = 80;

    // Prefer using HSLuv, not the browser's built-in vanilla HSL handling. This
    // is because this function chooses hue/lightness uniform at random, but HSL
    // is not perceptually uniform.
    // See https://www.boronine.com/2012/03/26/Color-Spaces-for-Human-Beings/.
    const base = new HSLuvColor({
      h: hue,
      s: saturation,
      l: hash(seed + 'x', 40) + 40,
    });
    const variant = new HSLuvColor({h: hue, s: saturation, l: 30});
    const colorScheme = makeColorScheme(base, variant);

    proceduralColorCache.set(seed, colorScheme);

    return colorScheme;
  }
}

export function colorForState(state: string): ColorScheme {
  if (state === 'Running') {
    return DARK_GREEN;
  } else if (state.startsWith('Runnable')) {
    return LIME_GREEN;
  } else if (state.includes('Uninterruptible Sleep')) {
    if (state.includes('non-IO')) {
      return DESAT_RED;
    }
    return ORANGE;
  } else if (state.includes('Dead')) {
    return GRAY;
  } else if (state.includes('Sleeping') || state.includes('Idle')) {
    return TRANSPARENT_WHITE;
  }
  return INDIGO;
}

export function colorForTid(tid: number): ColorScheme {
  return materialColorScheme(tid.toString());
}

export function colorForThread(thread?: {
  pid?: number;
  tid: number;
}): ColorScheme {
  if (thread === undefined) {
    return GRAY;
  }
  const tid = thread.pid ?? thread.tid;
  return colorForTid(tid);
}

export function colorForCpu(cpu: number): Color {
  if (USE_CONSISTENT_COLORS.get()) {
    return materialColorScheme(cpu.toString()).base;
  } else {
    const hue = (128 + 32 * cpu) % 256;
    return new HSLColor({h: hue, s: 50, l: 50});
  }
}

export function randomColor(): string {
  const rand = pseudoRand(randColourState);
  if (USE_CONSISTENT_COLORS.get()) {
    return materialColorScheme(rand.toString()).base.cssString;
  } else {
    // 40 different random hues 9 degrees apart.
    const hue = Math.floor(rand * 40) * 9;
    return '#' + hsl.hex([hue, 90, 30]);
  }
}

export function getColorForSlice(sliceName: string): ColorScheme {
  const name = sliceName.replace(/( )?\d+/g, '');
  if (USE_CONSISTENT_COLORS.get()) {
    return materialColorScheme(name);
  } else {
    return proceduralColorScheme(name);
  }
}

export function colorForFtrace(name: string): ColorScheme {
  return materialColorScheme(name);
}

export function getColorForSample(callsiteId: number): ColorScheme {
  if (USE_CONSISTENT_COLORS.get()) {
    return materialColorScheme(String(callsiteId));
  } else {
    return proceduralColorScheme(String(callsiteId));
  }
}
