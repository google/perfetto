// Copyright (C) 2022 The Android Open Source Project
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

import {hexToHsluv, hsluvToHex} from 'hsluv';

class HsluvCache {
  storage = new Map<number, string>();

  get(hue: number, saturation: number, lightness: number): string {
    const key = hue * 1e6 + saturation * 1e3 + lightness;
    const value = this.storage.get(key);

    if (value === undefined) {
      const computed = hsluvToHex([hue, saturation, lightness]);
      this.storage.set(key, computed);
      return computed;
    }

    return value;
  }
}

const cache = new HsluvCache();

export function cachedHsluvToHex(
    hue: number, saturation: number, lightness: number): string {
  return cache.get(hue, saturation, lightness);
}

// A mapping of slice colors to contrasting colors
// suitable for rendering text on them. Keys and
// values are both hex codes of the form #rrggbb.
const contrastingTextColorCodeCache = new Map<string, string>();

// Obtain a color code contrasting to the given |color|
// that is suitable for painting text on it.
// @param color hex code in the form #rrggbb of
//   something like a slice rendered in the track
// @returns a color hex code for contrasting text,
//   which will either be white for a dark |color|
//   or a dark grey-black for a light |color|
export function contrastingTextColorCode(color: string): string {
  let result = contrastingTextColorCodeCache.get(color);
  if (!result) {
    const lightness = hexToHsluv(color)[2];
    result = lightness > 65 ? '#202020' : '#ffffff';
    contrastingTextColorCodeCache.set(color, result);
  }
  return result;
}
