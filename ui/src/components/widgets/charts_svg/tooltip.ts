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

import {classNames} from '../../../base/classnames';
import {CursorTooltip} from '../../../widgets/cursor_tooltip';
import m from 'mithril';
import {PopupPosition} from '../../../widgets/popup';
import {HTMLAttrs} from '../../../widgets/common';

export interface ChartTooltipRowAttrs {
  readonly name: string;
  readonly value: string;
  /** Optional colour swatch (CSS colour). */
  readonly swatch?: string;
  /** Bold this row. */
  readonly tweak?: 'emphasis' | 'muted';
}

export const ChartTooltip = {
  view({attrs, children}: m.Vnode<HTMLAttrs>) {
    const {className, ...rest} = attrs;
    return m(
      CursorTooltip,
      {
        ...rest,
        className: classNames(className, 'pf-chart-svg__tooltip'),
        position: PopupPosition.RightStart,
        offset: 20,
      },
      m('.pf-chart-svg__tooltip-content', children),
    );
  },
  Header: {
    view({children}: m.Vnode) {
      return m('.pf-chart-svg__tooltip-header', children);
    },
  },
  Row: {
    view({attrs}: m.Vnode<ChartTooltipRowAttrs>) {
      const {name, value, tweak, swatch} = attrs;
      return m(
        '.pf-chart-svg__tooltip-row',
        {
          className: classNames(
            tweak === 'emphasis' && 'pf-chart-svg__tooltip-row--hovered',
            tweak === 'muted' && 'pf-chart-svg__tooltip-row--muted',
          ),
        },
        swatch &&
          m('.pf-chart-svg__tooltip-swatch', {
            style: {backgroundColor: swatch},
          }),
        m('.pf-chart-svg__tooltip-name', name),
        m('.pf-chart-svg__tooltip-value', value),
      );
    },
  },
};
