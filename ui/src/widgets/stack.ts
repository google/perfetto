// Copyright (C) 2025 The Android Open Source Project
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

import './stack.scss';
import m from 'mithril';
import {classNames} from '../base/classnames';
import {classForSpacing, type HTMLAttrs, type Spacing} from './common';

export interface StackAttrs extends HTMLAttrs {
  readonly orientation?: 'horizontal' | 'vertical';
  readonly fillHeight?: boolean;
  readonly spacing?: Spacing;
  readonly wrap?: boolean;
  readonly inline?: boolean;
}

/**
 * Stack lays children out in a column (or a row with
 * `orientation: 'horizontal'`) with a consistent gap. Use it as the
 * general-purpose layout container for stacking blocks of content. For
 * line-like rows of labels and controls, prefer Inline.
 */
export const Stack: m.Component<StackAttrs> = {
  view({attrs, children}) {
    const {
      orientation = 'vertical',
      fillHeight = false,
      spacing = 'medium',
      className,
      wrap,
      inline,
      ...htmlAttrs
    } = attrs;
    return m(
      '.pf-stack',
      {
        className: classNames(
          orientation === 'horizontal' && 'pf-stack--horiz',
          fillHeight && 'pf-stack--fill-height',
          classForSpacing(spacing),
          wrap && 'pf-stack--wrap',
          inline && 'pf-stack--inline',
          className,
        ),
        ...htmlAttrs,
      },
      children,
    );
  },
};

export interface InlineAttrs extends HTMLAttrs {
  readonly spacing?: Spacing;
  readonly wrap?: boolean;
  readonly inline?: boolean;
}

/**
 * Inline lays children out in a horizontal row, aligned on their text
 * baseline. Use it for line-like content such as a label, control and help
 * icon, or a row of buttons. Unlike Stack it is always horizontal.
 *
 * Use StackAuto to push trailing children to the end of the row. Non-text
 * children (icons, spinners) may need `align-self: center`.
 */
export const Inline: m.Component<InlineAttrs> = {
  view({attrs, children}) {
    const {spacing = 'medium', className, wrap, inline, ...htmlAttrs} = attrs;
    return m(
      '.pf-inline',
      {
        className: classNames(
          classForSpacing(spacing),
          wrap && 'pf-inline--wrap',
          inline && 'pf-inline--inline',
          className,
        ),
        ...htmlAttrs,
      },
      children,
    );
  },
};

/**
 * StackAuto is a container element designed to live inside a Stack. It will
 * automatically grow and shrink to fill the available space in the Stack.
 * This is useful for elements that should take up as much space as possible
 * without exceeding the bounds of the Stack.
 */
export const StackAuto: m.Component<HTMLAttrs> = {
  view({attrs, children}: m.CVnode<HTMLAttrs>) {
    return m('.pf-stack-auto', attrs, children);
  },
};

/**
 * StackFixed is a container element designed to live inside a Stack.
 * It will not grow or shrink, and will maintain its size based on its content.
 * This is useful for fixed-size elements that should not be resized.
 */
export const StackFixed: m.Component<HTMLAttrs> = {
  view({attrs, children}: m.CVnode<HTMLAttrs>) {
    return m('.pf-stack-fixed', attrs, children);
  },
};
