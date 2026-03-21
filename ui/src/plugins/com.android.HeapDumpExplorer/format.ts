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

export function fmtSize(n: number): string {
  if (n === 0) return '0';
  if (n >= 1_073_741_824) return `${(n / 1_073_741_824).toFixed(1)} GiB`;
  if (n >= 1_048_576) return `${(n / 1_048_576).toFixed(1)} MiB`;
  if (n >= 1024) return `${(n / 1024).toFixed(1)} KiB`;
  return n.toLocaleString();
}

export function fmtHex(id: number): string {
  return '0x' + id.toString(16).padStart(8, '0');
}

export function deltaBgClass(deltaKb: number): string {
  if (deltaKb === 0) return '';
  const abs = Math.abs(deltaKb);
  if (deltaKb > 0) {
    if (abs >= 50_000) return 'ah-delta-bg-pos-heavy';
    if (abs >= 10_000) return 'ah-delta-bg-pos-medium';
    if (abs >= 1_000) return 'ah-delta-bg-pos-light';
    return '';
  }
  if (abs >= 50_000) return 'ah-delta-bg-neg-heavy';
  if (abs >= 10_000) return 'ah-delta-bg-neg-medium';
  if (abs >= 1_000) return 'ah-delta-bg-neg-light';
  return '';
}

export function fmtDelta(deltaKb: number): string {
  if (deltaKb === 0) return '';
  const sign = deltaKb > 0 ? '+' : '\u2212';
  return `${sign}${fmtSize(Math.abs(deltaKb) * 1024)}`;
}

/** Format a byte-level delta (inspired by Android ahat's %+,d format). */
export function fmtSizeDelta(bytes: number): string {
  if (bytes === 0) return '';
  const sign = bytes > 0 ? '+' : '\u2212';
  return `${sign}${fmtSize(Math.abs(bytes))}`;
}

export function deltaBgClassBytes(bytes: number): string {
  return deltaBgClass(bytes / 1024);
}
