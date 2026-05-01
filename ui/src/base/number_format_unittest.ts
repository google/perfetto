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

import {formatNumber, toEngineeringNotation} from './number_format';

describe('toEngineeringNotation', () => {
  it('handles zero', () => expect(toEngineeringNotation(0)).toBe('0'));
  it('leaves small integers alone', () => {
    expect(toEngineeringNotation(85)).toBe('85');
    expect(toEngineeringNotation(1)).toBe('1');
    expect(toEngineeringNotation(999)).toBe('999');
  });
  it('formats thousands', () => {
    expect(toEngineeringNotation(3500)).toBe('3.5e3');
    expect(toEngineeringNotation(1000)).toBe('1e3');
  });
  it('formats millions and above', () => {
    expect(toEngineeringNotation(1e6)).toBe('1e6');
    expect(toEngineeringNotation(1.23e9)).toBe('1.23e9');
    expect(toEngineeringNotation(1e15)).toBe('1e15');
  });
  it('formats sub-1 values', () => {
    expect(toEngineeringNotation(0.001)).toBe('1e-3');
    expect(toEngineeringNotation(1.23e-6)).toBe('1.23e-6');
    expect(toEngineeringNotation(6e-3)).toBe('6e-3');
  });
  it('handles negative values', () => {
    expect(toEngineeringNotation(-3500)).toBe('-3.5e3');
    expect(toEngineeringNotation(-0.001)).toBe('-1e-3');
  });
  it('strips trailing zeros', () => {
    expect(toEngineeringNotation(3000)).toBe('3e3');
    expect(toEngineeringNotation(3.0e6)).toBe('3e6');
  });
});

describe('formatNumber — no unit', () => {
  it('uses engineering notation', () => {
    expect(formatNumber(0)).toBe('0');
    expect(formatNumber(85)).toBe('85');
    expect(formatNumber(3500)).toBe('3.5e3');
    expect(formatNumber(1.23e-6)).toBe('1.23e-6');
  });
});

describe('formatNumber — Hz (standard SI)', () => {
  it('formats sub-Hz values', () => {
    expect(formatNumber(1e-9, 'Hz')).toBe('1 nHz');
    expect(formatNumber(500e-6, 'Hz')).toBe('500 µHz');
    expect(formatNumber(0.001, 'Hz')).toBe('1 mHz');
  });
  it('formats Hz range', () => {
    expect(formatNumber(100, 'Hz')).toBe('100 Hz');
    expect(formatNumber(500e6, 'Hz')).toBe('500 MHz');
    expect(formatNumber(3.5e9, 'Hz')).toBe('3.5 GHz');
    expect(formatNumber(2.4e9, 'Hz')).toBe('2.4 GHz');
    expect(formatNumber(1e12, 'Hz')).toBe('1 THz');
  });
  it('falls back to engineering notation beyond T prefix', () => {
    expect(formatNumber(1e15, 'Hz')).toBe('1e15 Hz');
    expect(formatNumber(1e18, 'Hz')).toBe('1e18 Hz');
  });
  it('falls back to engineering notation below p prefix', () => {
    expect(formatNumber(1e-15, 'Hz')).toBe('1e-15 Hz');
  });
});

describe('formatNumber — W (standard SI)', () => {
  it('formats mW range', () => {
    expect(formatNumber(0.002, 'W')).toBe('2 mW');
    expect(formatNumber(1.5, 'W')).toBe('1.5 W');
    expect(formatNumber(1500, 'W')).toBe('1.5 KW');
  });
  it('falls back for very large values', () => {
    expect(formatNumber(1e15, 'W')).toBe('1e15 W');
  });
  it('handles zero', () => {
    expect(formatNumber(0, 'W')).toBe('0 W');
  });
});

describe('formatNumber — B (bytes, decimal, no sub-byte prefixes)', () => {
  it('formats byte range', () => {
    expect(formatNumber(500, 'B')).toBe('500 B');
    expect(formatNumber(1500, 'B')).toBe('1.5 KB');
    expect(formatNumber(1e6, 'B')).toBe('1 MB');
    expect(formatNumber(1.5e9, 'B')).toBe('1.5 GB');
    expect(formatNumber(2e12, 'B')).toBe('2 TB');
  });
  it('falls back to engineering notation above T', () => {
    expect(formatNumber(1e15, 'B')).toBe('1e15 B');
  });
  it('falls back to engineering notation for sub-1 values', () => {
    expect(formatNumber(0.5, 'B')).toBe('0.5 B');
    expect(formatNumber(0.001, 'B')).toBe('0.001 B');
  });
  it('handles zero', () => {
    expect(formatNumber(0, 'B')).toBe('0 B');
  });
  it('does not produce mB or uB', () => {
    expect(formatNumber(0.001, 'B')).not.toContain('mB');
    expect(formatNumber(1e-6, 'B')).not.toContain('µB');
  });
});

describe('formatNumber — s (seconds, sub-second prefixes only)', () => {
  it('formats sub-second range', () => {
    expect(formatNumber(1e-12, 's')).toBe('1 ps');
    expect(formatNumber(1e-9, 's')).toBe('1 ns');
    expect(formatNumber(1.5e-6, 's')).toBe('1.5 µs');
    expect(formatNumber(500e-3, 's')).toBe('500 ms');
  });
  it('formats seconds range', () => {
    expect(formatNumber(1, 's')).toBe('1 s');
    expect(formatNumber(60, 's')).toBe('60 s');
    expect(formatNumber(999, 's')).toBe('999 s');
  });
  it('falls back to engineering notation for large values', () => {
    expect(formatNumber(1000, 's')).toBe('1e3 s');
    expect(formatNumber(12e6, 's')).toBe('12e6 s');
  });
  it('falls back to engineering notation below ps', () => {
    expect(formatNumber(1e-15, 's')).toBe('1e-15 s');
  });
  it('handles zero', () => {
    expect(formatNumber(0, 's')).toBe('0 s');
  });
  it('does not produce Ms or Gs', () => {
    expect(formatNumber(1e6, 's')).not.toContain('Ms');
  });
});

describe('formatNumber — unknown unit', () => {
  it('uses engineering notation with unit appended', () => {
    expect(formatNumber(3e6, 'fps')).toBe('3e6 fps');
    expect(formatNumber(85, '%')).toBe('85 %');
    expect(formatNumber(0.5, 'rpm')).toBe('0.5 rpm');
  });
});

describe('formatNumber — unit aliases', () => {
  it('bytes alias formats like B', () => {
    expect(formatNumber(500, 'bytes')).toBe('500 B');
    expect(formatNumber(1500, 'bytes')).toBe('1.5 KB');
    expect(formatNumber(1e9, 'bytes')).toBe('1 GB');
  });
  it('seconds alias formats like s', () => {
    expect(formatNumber(1e-3, 'seconds')).toBe('1 ms');
    expect(formatNumber(1, 'seconds')).toBe('1 s');
  });
  it('hertz alias formats like Hz', () => {
    expect(formatNumber(2.4e9, 'hertz')).toBe('2.4 GHz');
  });
  it('watts alias formats like W', () => {
    expect(formatNumber(0.002, 'watts')).toBe('2 mW');
  });
});
