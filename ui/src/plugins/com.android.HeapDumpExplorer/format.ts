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

// Byte formatting shared by the heap dump views and the Memory Explorer.

import {formatBytesIec, type BytesFormatOptions} from '../../base/bytes_format';

// Auto-scaled size, e.g. "1.5 MiB".
export function fmtSize(n: number): string {
  return formatBytesIec(n);
}

// Fixed-unit size (MiB unless overridden), so values in one table line up.
export function formatBytes(
  bytes: number,
  options?: BytesFormatOptions,
): string {
  return formatBytesIec(bytes, {
    ...options,
    forceUnit: options?.forceUnit ?? 'MiB',
  });
}

// Signed byte delta, e.g. "+1.20 MiB" / "-0.50 MiB" / "±0.00 MiB".
export function formatDelta(
  bytes: number,
  options?: BytesFormatOptions,
): string {
  if (bytes === 0) return `±${formatBytes(0, options)}`;
  const sign = bytes > 0 ? '+' : '-';
  return `${sign}${formatBytes(Math.abs(bytes), options)}`;
}

// Red when a value grew, green when it shrank, default at zero.
export function deltaColor(n: number): string | undefined {
  if (n > 0) return 'var(--pf-color-danger)';
  if (n < 0) return 'var(--pf-color-success)';
  return undefined;
}

// Signed integer delta, e.g. "+1,024" / "-7".
export function formatCountDelta(n: number): string {
  return `${n > 0 ? '+' : ''}${n.toLocaleString()}`;
}

export function fmtHex(id: number): string {
  return '0x' + id.toString(16).padStart(8, '0');
}
