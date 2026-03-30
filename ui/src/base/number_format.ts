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

type Formatter = (value: number, unit: string) => string;

// Per-unit formatter registry. Maps a unit string (or the empty string for
// unitless values) to a formatting function.
// prettier-ignore
const FORMATTERS: Record<string, Formatter> = {
  '': formatEngineering,   // no unit: "85", "3.5e3", "1.23e-6"
  'Hz': formatStandard,      // "3 GHz", "500 MHz", "100 Hz"
  'W': formatStandard,      // "2 mW", "1.5 GW"
  'V': formatStandard,      // "3.3 V", "1 mV"
  'A': formatStandard,      // "500 mA", "2 uA"
  'J': formatStandard,      // "1.2 mJ", "4 GJ"
  'B': formatBytes,         // "1.5 KB", "2 GB"; sub-byte falls back to engineering
  'b': formatBytes,         // "1.5 Kb", "2 Gb"; sub-bit falls back to engineering
  's': formatSeconds,       // "500 ms", "1.5 us"; large values: "12e3 s"
};

// Maps long-form unit names to their canonical symbol in FORMATTERS.
// prettier-ignore
const UNIT_ALIASES: Record<string, string> = {
  'bytes': 'B',
  'bits': 'b',
  'seconds': 's',
  'hertz': 'Hz',
  'watts': 'W',
  'volts': 'V',
  'amps': 'A',
  'joules': 'J',
};

// Formats a number with optional unit, dispatching to the appropriate
// unit-aware formatter.
//
// Examples:
//   formatNumber(3.5e9, 'Hz') → "3.5 GHz"
//   formatNumber(0.001, 's')  → "1 ms"
//   formatNumber(1.5e9, 'B')  → "1.5 GB"
//   formatNumber(1500, 'bytes') → "1.5 KB"
//   formatNumber(3500)        → "3.5e3"
//   formatNumber(85, '%')     → "85 %"
export function formatNumber(value: number, unit?: string): string {
  const raw = unit ?? '';
  const key = UNIT_ALIASES[raw] ?? raw;
  const formatter = FORMATTERS[key] ?? formatUnknownUnit;
  return formatter(value, key);
}

// No unit: engineering notation with powers-of-1000 exponents.
// E.g. 3500 → "3.5e3", 0.001 → "1e-3", 85 → "85", 0 → "0".
function formatEngineering(value: number, _unit: string): string {
  return toEngineeringNotation(value);
}

// Standard SI prefixes (p, n, u, m, (none), K, M, G, T).
// Falls back to engineering notation for values outside this range.
// The prefix is attached to the unit symbol: "3 GHz", "2 mW", "1e15 W".
function formatStandard(value: number, unit: string): string {
  if (value === 0) return `0 ${unit}`;
  const {mantissa, prefix} = siPrefix(value, SI_METRIC_PREFIXES);
  if (inPrefixRange(mantissa)) return `${fmt(mantissa)} ${prefix}${unit}`;
  return `${toEngineeringNotation(value)} ${unit}`;
}

// Decimal byte/bit prefixes (K, M, G, T only — no sub-byte prefixes).
// Sub-1 values are shown directly (no mB/µB). Falls back to engineering
// notation outside the prefix range.
function formatBytes(value: number, unit: string): string {
  if (value === 0) return `0 ${unit}`;
  if (Math.abs(value) < 1) return `${fmt(value)} ${unit}`;
  const {mantissa, prefix} = siPrefix(value, BYTE_PREFIXES);
  if (inPrefixRange(mantissa)) return `${fmt(mantissa)} ${prefix}${unit}`;
  return `${toEngineeringNotation(value)} ${unit}`;
}

// Sub-second SI prefixes (ps, ns, us, ms, s). Falls back to engineering
// notation for values outside this range (e.g. "1e6 s" not "1 Ms").
function formatSeconds(value: number, unit: string): string {
  if (value === 0) return `0 ${unit}`;
  const absV = Math.abs(value);
  if (absV < 1) {
    const {mantissa, prefix} = siPrefix(value, TIME_SUB_PREFIXES);
    if (inPrefixRange(mantissa)) return `${fmt(mantissa)} ${prefix}${unit}`;
    return `${toEngineeringNotation(value)} ${unit}`;
  }
  if (absV < 1000) return `${fmt(value)} ${unit}`;
  return `${toEngineeringNotation(value)} ${unit}`;
}

// Unknown unit: engineering notation with the unit appended.
// E.g. formatNumber(3e6, 'fps') → "3e6 fps".
function formatUnknownUnit(value: number, unit: string): string {
  return `${toEngineeringNotation(value)} ${unit}`;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Standard metric SI prefixes.
const SI_METRIC_PREFIXES: [number, string][] = [
  [1e-12, 'p'],
  [1e-9, 'n'],
  [1e-6, 'µ'],
  [1e-3, 'm'],
  [1, ''],
  [1e3, 'K'],
  [1e6, 'M'],
  [1e9, 'G'],
  [1e12, 'T'],
];

// Decimal byte/bit prefixes (KB = 1000 B, MB = 1e6 B, etc.), upward only.
//
// We deliberately use decimal (SI) prefixes rather than binary (IEC) prefixes
// (KiB = 1024 B, MiB = 2^20 B, etc.) for two reasons:
//   1. Round numbers: a counter value of exactly 1,000,000 displays as "1 MB"
//      rather than the confusing "976.6 KiB".
//   2. Consistency: the SI prefix system used everywhere else in this file
//      (Hz, W, s, …) is base-10, so bytes should match.
//
// The sub-byte direction (mB, µB, …) is intentionally omitted; fractional
// bytes are shown as plain decimals instead (e.g. "0.5 B").
const BYTE_PREFIXES: [number, string][] = [
  [1, ''],
  [1e3, 'K'],
  [1e6, 'M'],
  [1e9, 'G'],
  [1e12, 'T'],
];

// Sub-second time prefixes only (ns, us, ms, s). No mega-seconds etc.
const TIME_SUB_PREFIXES: [number, string][] = [
  [1e-12, 'p'],
  [1e-9, 'n'],
  [1e-6, 'µ'],
  [1e-3, 'm'],
  [1, ''],
];

// Returns true if a mantissa is within the expressible prefix range [0.01, 1000).
// Outside this range the value should fall back to engineering notation.
function inPrefixRange(mantissa: number): boolean {
  const abs = Math.abs(mantissa);
  return abs === 0 || (abs >= 0.01 && abs < 1000);
}

// Selects the largest prefix whose multiplier does not exceed abs(value) and
// returns the scaled mantissa and prefix string.
function siPrefix(
  value: number,
  prefixes: [number, string][],
): {mantissa: number; prefix: string} {
  let multiplier = prefixes[0][0];
  let prefix = prefixes[0][1];
  const absV = Math.abs(value);
  for (const [m, p] of prefixes) {
    if (m > absV) break;
    [multiplier, prefix] = [m, p];
  }
  return {mantissa: value / multiplier, prefix};
}

// Formats a mantissa to 3 significant figures, stripping trailing zeros.
function fmt(n: number): string {
  return parseFloat(n.toPrecision(3)).toString();
}

// Formats n in engineering notation (powers of 10 in multiples of 3),
// stripping trailing zeros. Values in [0.01, 1000) are shown directly.
// E.g. 3500 → "3.5e3", 0.001 → "1e-3", 85 → "85", 0.5 → "0.5".
export function toEngineeringNotation(n: number): string {
  if (n === 0) return '0';
  const abs = Math.abs(n);
  if (abs >= 0.01 && abs < 1000) {
    return parseFloat(n.toPrecision(3)).toString();
  }
  const exp = Math.floor(Math.log10(abs));
  const engExp = Math.floor(exp / 3) * 3;
  const mantissa = n / Math.pow(10, engExp);
  const mantissaStr = parseFloat(mantissa.toPrecision(3)).toString();
  return `${mantissaStr}e${engExp}`;
}

// Scales n to the largest appropriate SI prefix and returns a rounded integer
// label and the prefix string. Kept for backward compatibility.
export function toLabelAndPrefix(n: number): {label: string; prefix: string} {
  if (n === 0) return {label: '0', prefix: ''};
  const {mantissa, prefix} = siPrefix(n, SI_METRIC_PREFIXES);
  return {label: `${Math.round(mantissa)}`, prefix};
}
