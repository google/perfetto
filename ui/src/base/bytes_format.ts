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

// Human-readable byte-size formatters.
//
// Two flavours, because the right base depends on what's being measured:
//   - formatBytesIec: base-1024 with IEC units (KiB/MiB/GiB). Use for RAM,
//     heap and other memory sizes — the kernel and allocators count in powers
//     of two.
//   - formatBytesSi: base-1000 with SI units (KB/MB/GB). Use for on-disk
//     file sizes, network transfer and anything quoted decimally.
//
// Both keep the sign, render whole bytes without a decimal point ("512 B"),
// and otherwise show two decimals ("1.50 MiB").
//
// By default the unit is auto-scaled to the largest that keeps the value >= 1.
// Pass a `forceUnit` ('B' or one of the IEC/SI unit strings) to pin every value
// to the same unit instead — useful for aligned/comparable table columns.

const IEC_UNITS = ['KiB', 'MiB', 'GiB', 'TiB', 'PiB'] as const;
const SI_UNITS = ['KB', 'MB', 'GB', 'TB', 'PB'] as const;

export type IecUnit = 'B' | (typeof IEC_UNITS)[number];
export type SiUnit = 'B' | (typeof SI_UNITS)[number];

export interface BytesFormatOptions {
  forceUnit?: IecUnit | SiUnit;
  fractionDigits?: number;
}

function formatScaled(
  bytes: number,
  base: number,
  units: readonly string[],
  forceUnit?: string,
  fractionDigits = 1,
): string {
  const sign = bytes < 0 ? '-' : '';
  const absBytes = Math.abs(bytes);

  // Forced unit: skip auto-scaling and render every value in this unit.
  if (forceUnit !== undefined) {
    if (forceUnit === 'B') return `${sign}${absBytes} B`;
    const i = units.indexOf(forceUnit);
    const v = absBytes / Math.pow(base, i + 1);
    return `${sign}${v.toLocaleString(undefined, {
      minimumFractionDigits: fractionDigits,
      maximumFractionDigits: fractionDigits,
    })} ${forceUnit}`;
  }

  if (absBytes < base) return `${sign}${absBytes} B`;
  let v = absBytes / base;
  let i = 0;
  while (v >= base && i < units.length - 1) {
    v /= base;
    i++;
  }
  return `${sign}${v.toLocaleString(undefined, {
    minimumFractionDigits: fractionDigits,
    maximumFractionDigits: fractionDigits,
  })} ${units[i]}`;
}

// Base-1024 with IEC units: "512 B", "1.50 KiB", "2.00 MiB". Use for memory.
export function formatBytesIec(
  bytes: number,
  options?: BytesFormatOptions,
): string {
  return formatScaled(
    bytes,
    1024,
    IEC_UNITS,
    options?.forceUnit,
    options?.fractionDigits,
  );
}

// Base-1000 with SI units: "512 B", "1.50 KB", "2.00 MB". Use for file sizes.
export function formatBytesSi(
  bytes: number,
  options?: BytesFormatOptions,
): string {
  return formatScaled(
    bytes,
    1000,
    SI_UNITS,
    options?.forceUnit,
    options?.fractionDigits,
  );
}
