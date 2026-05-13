// Copyright (C) 2023 The Android Open Source Project
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
import {HTMLCheckboxAttrs} from './common';

export interface CheckboxAttrs extends HTMLCheckboxAttrs {
  // Optional text to show to the right of the checkbox.
  readonly label?: m.Children;

  // Optional text to show to the left of the checkbox.
  readonly labelLeft?: m.Children;

  // Whether to render as a switch instead of a checkbox.
  readonly variant?: 'checkbox' | 'switch';
}

export class Checkbox implements m.ClassComponent<CheckboxAttrs> {
  view({attrs}: m.CVnode<CheckboxAttrs>) {
    const {
      label,
      labelLeft,
      disabled,
      checked,
      className,
      variant,
      ...htmlAttrs
    } = attrs;
    const classes = classNames(disabled && 'pf-disabled', className);

    // The default checkbox is removed and an entirely new one created inside
    // the span element in CSS.
    return m(
      'label.pf-checkbox',
      {
        ...htmlAttrs,
        className: classes,
      },
      labelLeft !== undefined && m('span.pf-checkbox__label-left', labelLeft),
      m('input[type=checkbox]', {disabled, checked}),
      variant === 'switch'
        ? m('span.pf-checkbox__box.pf-checkbox__box--switch')
        : m(
            'span.pf-checkbox__box.pf-checkbox__box--check',
            m(
              'svg.pf-checkbox__tick',
              {
                viewBox: '0 0 12 12',
                fill: 'none',
                xmlns: 'http://www.w3.org/2000/svg',
              },
              m('path', {
                'd': 'M2.5 6.5L5 9L9.5 3.5',
                'stroke': 'currentColor',
                'stroke-width': '2',
                'stroke-linecap': 'round',
                'stroke-linejoin': 'round',
              }),
            ),
          ),
      label !== undefined && m('span.pf-checkbox__label', label),
    );
  }
}
