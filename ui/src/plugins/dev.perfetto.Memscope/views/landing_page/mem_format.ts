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

// Widget helpers shared between the Memscope overview views. The byte
// formatters live in the heap dump explorer's format module.

import m from 'mithril';
import {Billboard} from '../../components/billboard';
import {
  deltaColor,
  formatDelta,
} from '../../../com.android.HeapDumpExplorer/format';

export {
  deltaColor,
  formatBytes,
  formatCountDelta,
  formatDelta,
} from '../../../com.android.HeapDumpExplorer/format';

// A delta rendered as colored text (red up / green down). `text` defaults to
// formatDelta(n); pass a custom string for rates, counts or "Δ … vs baseline".
export function deltaText(n: number, text: string = formatDelta(n)): m.Child {
  return m('span', {style: {color: deltaColor(n)}}, text);
}

// One score card holding 1-2 stat groups (label above, value, optional
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
