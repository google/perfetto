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

import './billboard.scss';
import m from 'mithril';

// Billboard — a single "stat card" that surfaces one headline number (value +
// unit) with a label, an optional description, and an optional delta badge that
// colours itself green/red from the leading sign of `delta`. Use it to draw the
// eye to the handful of figures that matter on a dashboard or summary panel
// (e.g. total RSS, swap used, page-cache size). Lay several side by side with
// BillboardStrip for an at-a-glance metrics row.

export interface BillboardAttrs {
  // The primary value to display prominently. Accepts m.Children so callers
  // can pass the output of billboardBytes() which embeds a unit span.
  readonly value: m.Children;
  // Unit to display next to the value, e.g. "MB". Accepts m.Children for
  // flexibility, but typically the output of billboardBytes().
  readonly unit: m.Children;
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

// A single stat card. Lay several side by side in a BillboardStrip for a
// metrics row, or pack several Billboard.Sections into one card to pair related
// stats in a single tile.
export class Billboard implements m.ClassComponent<BillboardAttrs> {
  view({attrs}: m.Vnode<BillboardAttrs>) {
    const {value, unit, label, desc, delta, color} = attrs;

    let deltaEl: m.Children = null;
    if (delta !== undefined) {
      const dir = delta.startsWith('+')
        ? 'pf-memscope-billboard__delta--up'
        : delta.startsWith('-')
          ? 'pf-memscope-billboard__delta--down'
          : '';
      deltaEl = m('.pf-memscope-billboard__delta', {class: dir}, delta);
    }

    const style =
      color !== undefined
        ? {
            background: `color-mix(in srgb, ${color} 10%, var(--pf-color-background-secondary))`,
          }
        : undefined;

    return m('.pf-memscope-billboard', {style}, [
      m('.pf-memscope-billboard__value', [
        value,
        m('.pf-memscope-billboard__unit', unit),
        deltaEl,
      ]),
      m('.pf-memscope-billboard__label', label),
      desc !== undefined && m('.pf-memscope-billboard__desc', desc),
    ]);
  }
}

export namespace Billboard {
  export interface SectionAttrs {
    // Heading shown above the value (rendered uppercase).
    readonly label: m.Children;
    // The headline figure for this segment.
    readonly value: m.Children;
    // Optional muted caption shown beneath the value.
    readonly sub?: m.Children;
  }

  // One labelled segment within a Billboard card. Drop two or three directly
  // inside a `.pf-memscope-billboard` element and they lay out side by side, so
  // related figures share a single tile (uptime + OOM score, peak RSS + spike,
  // memory Δ + trend …) instead of needing a card each.
  export class Section implements m.ClassComponent<SectionAttrs> {
    view({attrs}: m.Vnode<SectionAttrs>) {
      return m('.pf-memscope-billboard__section', [
        m('.pf-memscope-billboard__label', attrs.label),
        m('.pf-memscope-billboard__value', attrs.value),
        attrs.sub !== undefined && m('.pf-memscope-billboard__sub', attrs.sub),
      ]);
    }
  }
}

// BillboardStrip — a horizontal flex row that lays out a set of Billboards with
// equal growth, so a group of related stats reads as one cohesive metrics band.
export const BillboardStrip: m.Component = {
  view({children}: m.Vnode) {
    return m('.pf-billboard-strip', children);
  },
};
