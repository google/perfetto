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

import {hsluvToRgb} from 'hsluv';
import {clamp} from '../base/math_utils';

// This file contains a library for working with colors in various color spaces
// and formats.

const LIGHTNESS_MIN = 0;
const LIGHTNESS_MAX = 100;

const SATURATION_MIN = 0;
const SATURATION_MAX = 100;

// Most color formats can be defined using 3 numbers in a standardized order, so
// this tuple serves as a compact way to store various color formats.
// E.g. HSL, RGB
type ColorTuple = [number, number, number];

// Definition of an HSL color with named fields.
interface HSL {
  readonly h: number; // 0-360
  readonly s: number; // 0-100
  readonly l: number; // 0-100
}

// Defines an interface to an immutable color object, which can be defined in
// any arbitrary format or color space and provides function to modify the color
// and conversions to CSS compatible style strings.
// Because this color object is effectively immutable, a new color object is
// returned when modifying the color, rather than editing the current object
// in-place.
// Also, because these objects are immutable, it's expected that readonly
// properties such as |cssString| are efficient, as they can be computed at
// creation time, so they may be used in the hot path (render loop).
export interface Color {
  readonly cssString: string;

  // The perceived brightness of the color using a weighted average of the
  // r, g and b channels based on human perception.
  readonly perceivedBrightness: number;

  // Bring up the lightness by |percent| percent.
  lighten(percent: number, max?: number): Color;

  // Bring down the lightness by |percent| percent.
  darken(percent: number, min?: number): Color;

  // Bring up the saturation by |percent| percent.
  saturate(percent: number, max?: number): Color;

  // Bring down the saturation by |percent| percent.
  desaturate(percent: number, min?: number): Color;

  // Set one or more HSL values.
  setHSL(hsl: Partial<HSL>): Color;

  setAlpha(alpha: number | undefined): Color;
}

// Common base class for HSL colors. Avoids code duplication.
abstract class HSLColorBase<T extends Color> {
  readonly hsl: ColorTuple;
  readonly alpha?: number;

  // Values are in the range:
  // Hue:        0-360
  // Saturation: 0-100
  // Lightness:  0-100
  // Alpha:      0-1
  constructor(init: ColorTuple | HSL | string, alpha?: number) {
    if (Array.isArray(init)) {
      this.hsl = init;
    } else if (typeof init === 'string') {
      const rgb = hexToRgb(init);
      this.hsl = rgbToHsl(rgb);
    } else {
      this.hsl = [init.h, init.s, init.l];
    }
    this.alpha = alpha;
  }

  // Subclasses should implement this to teach the base class how to create a
  // new object of the subclass type.
  abstract create(hsl: ColorTuple | HSL, alpha?: number): T;

  lighten(amount: number, max = LIGHTNESS_MAX): T {
    const [h, s, l] = this.hsl;
    const newLightness = clamp(l + amount, LIGHTNESS_MIN, max);
    return this.create([h, s, newLightness], this.alpha);
  }

  darken(amount: number, min = LIGHTNESS_MIN): T {
    const [h, s, l] = this.hsl;
    const newLightness = clamp(l - amount, min, LIGHTNESS_MAX);
    return this.create([h, s, newLightness], this.alpha);
  }

  saturate(amount: number, max = SATURATION_MAX): T {
    const [h, s, l] = this.hsl;
    const newSaturation = clamp(s + amount, SATURATION_MIN, max);
    return this.create([h, newSaturation, l], this.alpha);
  }

  desaturate(amount: number, min = SATURATION_MIN): T {
    const [h, s, l] = this.hsl;
    const newSaturation = clamp(s - amount, min, SATURATION_MAX);
    return this.create([h, newSaturation, l], this.alpha);
  }

  setHSL(hsl: Partial<HSL>): T {
    const [h, s, l] = this.hsl;
    return this.create({h, s, l, ...hsl}, this.alpha);
  }

  setAlpha(alpha: number | undefined): T {
    return this.create(this.hsl, alpha);
  }
}

// Describes a color defined in standard HSL color space.
export class HSLColor extends HSLColorBase<HSLColor> implements Color {
  readonly cssString: string;
  readonly perceivedBrightness: number;

  // Values are in the range:
  // Hue:        0-360
  // Saturation: 0-100
  // Lightness:  0-100
  // Alpha:      0-1
  constructor(hsl: ColorTuple | HSL | string, alpha?: number) {
    super(hsl, alpha);

    const [r, g, b] = hslToRgb(...this.hsl);

    this.perceivedBrightness = perceivedBrightness(r, g, b);

    if (this.alpha === undefined) {
      this.cssString = `rgb(${r} ${g} ${b})`;
    } else {
      this.cssString = `rgb(${r} ${g} ${b} / ${this.alpha})`;
    }
  }

  create(values: ColorTuple | HSL, alpha?: number | undefined): HSLColor {
    return new HSLColor(values, alpha);
  }
}

// Describes a color defined in HSLuv color space.
// See: https://www.hsluv.org/
export class HSLuvColor extends HSLColorBase<HSLuvColor> implements Color {
  readonly cssString: string;
  readonly perceivedBrightness: number;

  constructor(hsl: ColorTuple | HSL, alpha?: number) {
    super(hsl, alpha);

    const rgb = hsluvToRgb(this.hsl);
    const r = Math.floor(rgb[0] * 255);
    const g = Math.floor(rgb[1] * 255);
    const b = Math.floor(rgb[2] * 255);

    this.perceivedBrightness = perceivedBrightness(r, g, b);

    if (this.alpha === undefined) {
      this.cssString = `rgb(${r} ${g} ${b})`;
    } else {
      this.cssString = `rgb(${r} ${g} ${b} / ${this.alpha})`;
    }
  }

  create(raw: ColorTuple | HSL, alpha?: number | undefined): HSLuvColor {
    return new HSLuvColor(raw, alpha);
  }
}

// Hue: 0-360
// Saturation: 0-100
// Lightness: 0-100
// RGB: 0-255
export function hslToRgb(h: number, s: number, l: number): ColorTuple {
  h = h;
  s = s / SATURATION_MAX;
  l = l / LIGHTNESS_MAX;

  const c = (1 - Math.abs(2 * l - 1)) * s;
  const x = c * (1 - Math.abs(((h / 60) % 2) - 1));
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

export function hexToRgb(hex: string): ColorTuple {
  // Convert hex to RGB first
  let r: number = 0;
  let g: number = 0;
  let b: number = 0;

  if (hex.length === 4) {
    r = parseInt(hex[1] + hex[1], 16);
    g = parseInt(hex[2] + hex[2], 16);
    b = parseInt(hex[3] + hex[3], 16);
  } else if (hex.length === 7) {
    r = parseInt(hex.substring(1, 3), 16);
    g = parseInt(hex.substring(3, 5), 16);
    b = parseInt(hex.substring(5, 7), 16);
  }

  return [r, g, b];
}

export function rgbToHsl(rgb: ColorTuple): ColorTuple {
  let [r, g, b] = rgb;
  r /= 255;
  g /= 255;
  b /= 255;
  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  let h: number = (max + min) / 2;
  let s: number = (max + min) / 2;
  const l: number = (max + min) / 2;

  if (max === min) {
    h = s = 0; // achromatic
  } else {
    const d = max - min;
    s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
    switch (max) {
      case r:
        h = (g - b) / d + (g < b ? 6 : 0);
        break;
      case g:
        h = (b - r) / d + 2;
        break;
      case b:
        h = (r - g) / d + 4;
        break;
    }
    h /= 6;
  }

  return [h * 360, s * 100, l * 100];
}

// Return the perceived brightness of a color using a weighted average of the
// r, g and b channels based on human perception.
function perceivedBrightness(r: number, g: number, b: number): number {
  // YIQ calculation from https://24ways.org/2010/calculating-color-contrast
  return (r * 299 + g * 587 + b * 114) / 1000;
}

// Comparison function used for sorting colors.
export function colorCompare(a: Color, b: Color): number {
  return a.cssString.localeCompare(b.cssString);
}
