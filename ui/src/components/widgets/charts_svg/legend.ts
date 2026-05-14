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
import {classNames} from '../../../base/classnames';
import {HTMLAttrs} from '../../../widgets/common';

export interface ChartLegendEntryAttrs {
  readonly name: string;
  /** Optional trailing value (e.g. last data point). */
  readonly value?: string;
  /** Optional colour swatch (CSS colour). */
  readonly swatch?: string;
  /** Render with the hidden/struck-through style. */
  readonly hidden?: boolean;
  /** Click handler. When set, the entry shows a pointer cursor. */
  readonly onToggle?: () => void;
  /** Called when the mouse enters this legend entry */
  readonly onMouseEnter?: () => void;
  /** Called when the mouse leaves this legend entry */
  readonly onMouseLeave?: () => void;
}

/**
 * Compound legend component. Mirrors `ChartTooltip` — the outer
 * `ChartLegend` is just the styled container; `ChartLegend.Entry` is one
 * swatch+name+value row inside it.
 */
export const ChartLegend = {
  view({attrs, children}: m.Vnode<HTMLAttrs>) {
    const {className, ...rest} = attrs;
    return m(
      '.pf-chart-svg__legend',
      {...rest, className: classNames(className)},
      children,
    );
  },
  Entry: {
    view({attrs}: m.Vnode<ChartLegendEntryAttrs>) {
      const {
        name,
        value,
        swatch,
        hidden,
        onToggle,
        onMouseEnter,
        onMouseLeave,
      } = attrs;
      return m(
        '.pf-chart-svg__legend-entry',
        {
          className: classNames(hidden && 'pf-chart-svg__legend-entry--hidden'),
          style: onToggle ? {cursor: 'pointer'} : undefined,
          onclick: onToggle,
          onmouseenter: onMouseEnter,
          onmouseleave: onMouseLeave,
        },
        swatch !== undefined &&
          m('.pf-chart-svg__legend-swatch', {
            style: {backgroundColor: swatch},
          }),
        m('.pf-chart-svg__legend-name', name),
        value !== undefined && m('.pf-chart-svg__legend-value', value),
      );
    },
  },
};
