// Copyright (C) 2025 The Android Open Source Project
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
import {ColorScheme} from '../../base/color_scheme';
import {GRAY, GRAY_COLOR, makeColorScheme} from '../../components/colorizer';

export const CPU_SLICE_URI_PREFIX = '/sched_cpu';

// Helper function moved here as it's only used by the overlay.
export function uriForSchedTrack(cpu: number): string {
  return `${CPU_SLICE_URI_PREFIX}${cpu}`;
}

const DESAT_RED = makeColorScheme(new HSLColor([3, 30, 49]));
const DARK_GREEN = makeColorScheme(new HSLColor([120, 44, 34]));
const LIME_GREEN = makeColorScheme(new HSLColor([75, 55, 47]));
const TRANSLUCENT_GRAY = {
  base: new HSLColor([0, 1, 50], 0),
  variant: new HSLColor([0, 1, 50], 0.2),
  disabled: GRAY_COLOR,
  // Make the text invisible
  textBase: new HSLColor([0, 0, 0], 0),
  textVariant: new HSLColor([0, 0, 0], 0),
  textDisabled: new HSLColor([0, 0, 0], 0),
};
const LIGHT_GRAY = makeColorScheme(new HSLColor([0, 0, 70]));
const ORANGE = makeColorScheme(new HSLColor([36, 100, 50]));
const INDIGO = makeColorScheme(new HSLColor([231, 48, 48]));

export function colorForThreadState(state: string): ColorScheme {
  if (state === 'Created') {
    return LIGHT_GRAY;
  } else if (state === 'Running') {
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
    return TRANSLUCENT_GRAY;
  }
  return INDIGO;
}
