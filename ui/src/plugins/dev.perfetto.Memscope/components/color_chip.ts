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

import './color_chip.scss';
import m from 'mithril';

// ColorChip — a small rounded pill tinted with a caller-supplied colour, used
// to tag a row or legend entry with the same hue as its data series (memory
// category, process group, chart slice). The raw colour is softened against the
// page background so the chip reads as a tint rather than a saturated block;
// use chipColor() to compute that exact softened colour elsewhere (e.g. for a
// matching chart series) so the chip and the thing it labels stay in sync.

export interface ColorChipAttrs {
  // Hex color string - can be any valid CSS.
  readonly color?: string;
}

/**
 * Returns the pastel-mixed CSS color used by chips. Use this to keep other
 * UI surfaces (e.g. chart series) visually in sync with the chips.
 */
export function chipColor(color: string): string {
  return `color-mix(in srgb, ${color} 75%, var(--pf-color-background))`;
}

export const ColorChip: m.Component<ColorChipAttrs> = {
  view({attrs, children}) {
    const {color = 'var(--pf-color-text)'} = attrs;
    return m(
      '.pf-memscope-color-chip',
      {style: {'--pf-memscope-chip-color': color}},
      children,
    );
  },
};
