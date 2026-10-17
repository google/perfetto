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

import {clamp} from '../../base/math_utils';

const SI_PREFIXES: ReadonlyArray<readonly [number, string]> = [
  [1e12, 'T'],
  [1e9, 'G'],
  [1e6, 'M'],
  [1e3, 'K'],
  [1, ''],
  [1e-3, 'm'],
  [1e-6, 'µ'],
  [1e-9, 'n'],
  [1e-12, 'p'],
];

/**
 * Computes nice tick marks within [min, max] using 1/2/5 x 10^k steps.
 */
export function niceTicks(
  min: number,
  max: number,
  targetCount: number,
): number[] {
  if (!Number.isFinite(min) || !Number.isFinite(max) || targetCount <= 0) {
    return [];
  }
  const lo = Math.min(min, max);
  const hi = Math.max(min, max);
  const span = hi - lo;
  if (span <= 0) return [lo];

  const target = clamp(Math.round(targetCount), 1, 100);
  const rawStep = span / target;
  if (!Number.isFinite(rawStep) || rawStep <= 0) return [lo];

  const exp = Math.floor(Math.log10(rawStep));
  const frac = rawStep / Math.pow(10, exp);
  const mult = frac > 7.5 ? 10 : frac > 3.5 ? 5 : frac > 1.5 ? 2 : 1;
  const step = mult * Math.pow(10, exp);
  if (!Number.isFinite(step) || step <= 0 || lo + step === lo) return [lo];

  const rLo = lo / step;
  const kMin = Math.ceil(
    rLo - Math.max(1, Math.abs(rLo)) * Number.EPSILON * 64,
  );
  const rHi = hi / step;
  const kMax = Math.floor(
    rHi + Math.max(1, Math.abs(rHi)) * Number.EPSILON * 64,
  );
  if (kMin > kMax) return [];

  const count = Math.min(kMax - kMin + 1, Math.max(100, target * 4));
  const ticks: number[] = [];
  const stepDecimals = Math.max(0, -Math.floor(Math.log10(step) + 1e-9));
  const epsLo = Math.max(1, Math.abs(lo)) * Number.EPSILON * 64;
  const epsHi = Math.max(1, Math.abs(hi)) * Number.EPSILON * 64;

  for (let i = 0; i < count; i++) {
    let val = (kMin + i) * step;
    if (stepDecimals > 0 && stepDecimals <= 14) {
      val = Number(val.toFixed(stepDecimals));
    }
    if (val === 0) val = 0;
    if (val < lo) {
      if (Math.abs(val - lo) <= epsLo) val = lo;
      else continue;
    }
    if (val > hi) {
      if (Math.abs(val - hi) <= epsHi) val = hi;
      else continue;
    }
    ticks.push(val);
  }
  return ticks;
}

/**
 * Formats a single tick value compactly given the tick step size.
 */
export function formatTick(value: number, step: number): string {
  if (!Number.isFinite(value)) return String(value);
  if (value === 0) return '0';

  const absVal = Math.abs(value);
  let chosenMultiplier = 1;
  let chosenPrefix = '';
  for (const [multiplier, prefix] of SI_PREFIXES) {
    if (absVal >= multiplier * 0.9999) {
      chosenMultiplier = multiplier;
      chosenPrefix = prefix;
      break;
    }
  }

  const scaledStep = step / chosenMultiplier;
  if (scaledStep >= 0.00099) {
    const decimals = clamp(-Math.floor(Math.log10(scaledStep) + 1e-9), 0, 6);
    const s = (value / chosenMultiplier).toFixed(decimals);
    const formatted = decimals > 0 ? parseFloat(s).toString() : s;
    return chosenPrefix.length > 0 ? `${formatted} ${chosenPrefix}` : formatted;
  }

  const stepDecimals = Math.max(0, -Math.floor(Math.log10(step) + 1e-9));
  return stepDecimals === 0
    ? Math.round(value).toString()
    : value.toFixed(Math.min(20, stepDecimals));
}

/**
 * Formats a value for detailed inspection / tooltips with full precision.
 */
export function formatValue(v: number): string {
  if (!Number.isFinite(v)) return String(v);
  if (v === 0) return '0';
  if (Number.isInteger(v)) return v.toLocaleString('en-US');

  const s = String(v);
  if (s.includes('e') || s.includes('E')) return s;
  const parts = s.split('.');
  return parts.length === 2
    ? `${parts[0].replace(/\B(?=(\d{3})+(?!\d))/g, ',')}.${parts[1]}`
    : s;
}

export interface AxisTicks {
  readonly ticks: number[];
  readonly step: number;
  readonly offset: number;
  readonly labels: string[];
}

export function computeAxisTicks(
  min: number,
  max: number,
  targetCount: number,
): AxisTicks {
  if (!Number.isFinite(min) || !Number.isFinite(max) || targetCount <= 0) {
    return {ticks: [], step: 0, offset: 0, labels: []};
  }
  const lo = Math.min(min, max);
  const hi = Math.max(min, max);
  const span = hi - lo;
  const ticks = niceTicks(lo, hi, targetCount);
  if (ticks.length === 0) {
    return {ticks: [], step: span > 0 ? span : 1, offset: 0, labels: []};
  }

  let step = ticks.length >= 2 ? ticks[1] - ticks[0] : span > 0 ? span : 1;
  const stepDecimals = Math.max(0, -Math.floor(Math.log10(step) + 1e-9));
  if (stepDecimals > 0 && stepDecimals <= 14) {
    step = Number(step.toFixed(stepDecimals));
  }

  const maxAbs = Math.max(Math.abs(lo), Math.abs(hi));
  let offset = 0;
  if (span > 0 && maxAbs / step >= 1e4) {
    const P = Math.pow(10, Math.ceil(Math.log10(span)));
    offset = Math.floor(lo / P) * P;
  }

  const labels = ticks.map((t) => {
    if (offset === 0) return formatTick(t, step);
    let relVal = t - offset;
    if (stepDecimals > 0 && stepDecimals <= 14) {
      relVal = Number(relVal.toFixed(stepDecimals));
    }
    return formatTick(relVal === 0 ? 0 : relVal, step);
  });

  return {ticks, step, offset, labels};
}

export function formatAxisOffset(offset: number): string {
  return formatValue(offset);
}

export function fitTickCount(
  min: number,
  max: number,
  maxTargetCount: number,
  maxLabelPx: (labels: string[]) => number,
  availablePx: number,
  minGapPx = 8,
): AxisTicks {
  let target = Math.max(2, Math.round(maxTargetCount));
  let result = computeAxisTicks(min, max, target);
  while (target > 2) {
    if (
      result.ticks.length * (maxLabelPx(result.labels) + minGapPx) <=
      availablePx
    ) {
      return result;
    }
    target--;
    result = computeAxisTicks(min, max, target);
  }
  return result;
}
