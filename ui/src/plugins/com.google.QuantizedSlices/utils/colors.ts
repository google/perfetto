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

import {HSLColor, HSLuvColor} from '../../../base/color';
import {hash} from '../../../base/hash';

const nameColorCache = new Map<string, string>();

export function nameColor(name: string): string {
  const seed = name.replace(/( )?\d+/g, '');
  if (nameColorCache.has(seed)) return nameColorCache.get(seed)!;
  const hue = hash(seed, 360);
  const lightness = hash(seed + 'x', 40) + 40;
  const color = new HSLuvColor([hue, 80, lightness]);
  nameColorCache.set(seed, color.cssString);
  return color.cssString;
}

export function isDark(): boolean {
  return !!document.querySelector('.pf-theme-provider--dark');
}

// Thread state colors — using Perfetto's HSLColor.
const STATE_RUNNING = new HSLColor([120, 44, 34]).cssString;
const STATE_RUNNABLE = new HSLColor([75, 55, 47]).cssString;
const STATE_IO_WAIT = new HSLColor([36, 100, 50]).cssString;
const STATE_NONIO = new HSLColor([3, 30, 49]).cssString;
const STATE_CREATED = new HSLColor([0, 0, 70]).cssString;
const STATE_UNKNOWN = new HSLColor([44, 63, 91]).cssString;
const STATE_DEAD = new HSLColor([0, 0, 62]).cssString;
const STATE_INDIGO = new HSLColor([231, 48, 48]).cssString;

export function stateColor(d: {
  state: string | null;
  io_wait: number | null;
}): string {
  const s = d.state;
  if (!s) return isDark() ? '#44444e' : STATE_UNKNOWN;
  if (s === 'Created') return STATE_CREATED;
  if (s === 'Running') return STATE_RUNNING;
  if (s.startsWith('Runnable')) return STATE_RUNNABLE;
  if (s.includes('Uninterruptible Sleep')) {
    if (s.includes('non-IO') || d.io_wait === 0) return STATE_NONIO;
    return STATE_IO_WAIT;
  }
  if (s.includes('Dead')) return STATE_DEAD;
  if (s.includes('Sleeping') || s.includes('Idle')) {
    return isDark() ? '#2a2a3a' : '#ffffff';
  }
  if (s.includes('Unknown')) return STATE_UNKNOWN;
  return STATE_INDIGO;
}

export function stateLabel(d: {
  state: string | null;
  io_wait: number | null;
}): string {
  if (!d.state) return 'Unknown';
  if (d.state === 'Uninterruptible Sleep') {
    if (d.io_wait === 1) return 'Unint. Sleep (IO)';
    if (d.io_wait === 0) return 'Unint. Sleep (non-IO)';
    return 'Unint. Sleep';
  }
  return d.state;
}
