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

import m from 'mithril';
import {classNames} from '../base/classnames';
import {classForSpacing, HTMLAttrs, Spacing} from './common';

interface StackAttrs extends HTMLAttrs {
  readonly orientation?: 'horizontal' | 'vertical';
  readonly fillHeight?: boolean;
  readonly spacing?: Spacing;
  readonly wrap?: boolean;
  readonly inline?: boolean;
}

export class Stack implements m.ClassComponent<StackAttrs> {
  view({attrs, children}: m.CVnode<StackAttrs>) {
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
  }
}

/**
 * StackAuto is a container element designed to live inside a Stack. It will
 * automatically grow and shrink to fill the available space in the Stack.
 * This is useful for elements that should take up as much space as possible
 * without exceeding the bounds of the Stack.
 */
export class StackAuto implements m.ClassComponent<HTMLAttrs> {
  view({attrs, children}: m.CVnode<HTMLAttrs>) {
    return m('.pf-stack-auto', attrs, children);
  }
}

/**
 * StackFixed is a container element designed to live inside a Stack.
 * It will not grow or shrink, and will maintain its size based on its content.
 * This is useful for fixed-size elements that should not be resized.
 */
export class StackFixed implements m.ClassComponent<HTMLAttrs> {
  view({attrs, children}: m.CVnode<HTMLAttrs>) {
    return m('.pf-stack-fixed', attrs, children);
  }
}
