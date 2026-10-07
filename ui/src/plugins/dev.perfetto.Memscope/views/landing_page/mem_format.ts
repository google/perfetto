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

// Small formatting/widget helpers shared between the Memscope overview views
// (the per-process overview page and the top-level trace overview cards).

import m from 'mithril';
import {
  formatBytesIec,
  type BytesFormatOptions,
} from '../../../../base/bytes_format';
import {Billboard} from '../../components/billboard';

// Memory sizes are always shown in mebibytes (base-1024) for consistency across
// the page, rather than auto-scaling to B/KiB/GiB. Pass an explicit forceUnit to
// override for a specific call.
const UNIT: BytesFormatOptions['forceUnit'] = 'MiB';

export function formatBytes(
  bytes: number,
  options?: BytesFormatOptions,
): string {
  return formatBytesIec(bytes, {
    ...options,
    forceUnit: options?.forceUnit ?? UNIT,
  });
}

// Formats a signed byte delta, e.g. "+1.20 MiB" / "-0.50 MiB" / "±0.00 MiB".
// Pass {fractionDigits} to control decimal places (default 2). Uses the
// current locale for grouping/decimals automatically.
export function formatDelta(
  bytes: number,
  options?: BytesFormatOptions,
): string {
  if (bytes === 0) return `±${formatBytes(0, options)}`;
  const sign = bytes > 0 ? '+' : '-';
  const absBytes = Math.abs(bytes);
  return `${sign}${formatBytes(absBytes, options)}`;
}

// Consistent delta coloring across the page: red when a value grew (up),
// green when it shrank (down), neutral (default text color) at zero. Matches
// the Billboard delta convention (--pf-color-danger / --pf-color-success).
export function deltaColor(n: number): string | undefined {
  if (n > 0) return 'var(--pf-color-danger)';
  if (n < 0) return 'var(--pf-color-success)';
  return undefined;
}

// Signed integer delta, e.g. "+1,024" / "-7".
export function formatCountDelta(n: number): string {
  return `${n > 0 ? '+' : ''}${n.toLocaleString()}`;
}

// A delta rendered as colored text (red up / green down). `text` defaults to
// formatDelta(n); pass a custom string for rates, counts or "Δ … vs baseline".
export function deltaText(n: number, text: string = formatDelta(n)): m.Child {
  return m('span', {style: {color: deltaColor(n)}}, text);
}

// One score card holding 1–2 stat groups (label above, value, optional
// sub-line). Used for the top billboard row and the per-section cards. Each
// group is a Billboard.Section, so multiple groups lay out side by side.
export function statCard(
  stats: {label: string; value: m.Children; sub?: m.Children}[],
): m.Child {
  return m(
    '.pf-memscope-billboard',
    stats.map((s) => m(Billboard.Section, s)),
  );
}
