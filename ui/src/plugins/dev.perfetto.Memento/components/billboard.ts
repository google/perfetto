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

import m from 'mithril';

export interface BillboardAttrs {
  // The primary value to display prominently. Accepts m.Children so callers
  // can pass the output of billboardKb() which embeds a unit span.
  readonly value: m.Children;
  // Short label displayed below the value.
  readonly label: string;
  // Optional longer description shown below the label.
  readonly desc?: string;
  // Optional delta string (e.g. "+12 MB"). Direction inferred from leading sign.
  readonly delta?: string;
  // Optional accent color (any valid CSS color). When set, applies a colored
  // left border and a subtle tinted background.
  readonly color?: string;
}

// A single stat card. Wrap multiple in billboards() for a row layout.
export function billboard(attrs: BillboardAttrs): m.Children {
  const {value, label, desc, delta, color} = attrs;

  let deltaEl: m.Children = null;
  if (delta !== undefined) {
    const dir = delta.startsWith('+')
      ? 'pf-memento-billboard__delta--up'
      : delta.startsWith('-')
        ? 'pf-memento-billboard__delta--down'
        : '';
    deltaEl = m('.pf-memento-billboard__delta', {class: dir}, delta);
  }

  const style =
    color !== undefined
      ? {
          borderLeftColor: color,
          borderLeftWidth: '3px',
          background: `color-mix(in srgb, ${color} 10%, var(--pf-color-background-secondary))`,
        }
      : undefined;

  return m(
    '.pf-memento-billboard',
    {style},
    m('.pf-memento-billboard__value', value, deltaEl),
    m('.pf-memento-billboard__label', label),
    desc !== undefined && m('.pf-memento-billboard__desc', desc),
  );
}

// Wraps multiple billboard() calls in a flex row container.
export function billboards(...cards: m.Children[]): m.Children {
  return m('.pf-memento-billboards', ...cards);
}
