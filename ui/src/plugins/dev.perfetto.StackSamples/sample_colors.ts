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

import {HSLColor} from '../../base/color';
import type {ColorScheme} from '../../base/color_scheme';
import {hash} from '../../base/hash';
import {GRAY, makeColorScheme} from '../../components/colorizer';

// Frames are colored by where they come from, consistently across all
// instant and flamechart tracks: binary, library, kernel, or unknown mapping.
// Categories are defined by std.stack_sample.mapping; shades use the mapping path,
// so symbolization and function names do not change a sample's origin color.
export const CATEGORY_BINARY = 0;
export const CATEGORY_LIBRARY = 1;
export const CATEGORY_KERNEL = 2;
export const CATEGORY_UNKNOWN = 3;

const CATEGORY_LABELS = ['Binary', 'Library', 'Kernel', 'Unknown'];
const CATEGORY_HUES = [217, 110, 30];
const SATURATION = 28;
const LIGHTNESS_BASE = 48;
const LIGHTNESS_JITTER = 8;

export function sampleCategoryLabel(category: number): string {
  return CATEGORY_LABELS[category] ?? 'Unknown';
}

const cache = new Map<string, ColorScheme>();

export function sampleColorScheme(category: number, name: string): ColorScheme {
  const hue = CATEGORY_HUES[category];
  if (hue === undefined) return GRAY;
  const key = `${category}#${name}`;
  let scheme = cache.get(key);
  if (scheme === undefined) {
    const lightness = LIGHTNESS_BASE + hash(name, LIGHTNESS_JITTER);
    scheme = makeColorScheme(new HSLColor([hue, SATURATION, lightness]));
    cache.set(key, scheme);
  }
  return scheme;
}
