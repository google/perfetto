// Copyright (C) 2024 The Android Open Source Project
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
import {HTMLAttrs, Intent, classForIntent} from './common';
import {Icon} from './icon';
import {Button} from './button';

export interface ChipAttrs extends HTMLAttrs {
  // Chips require a label.
  readonly label: m.Children;
  // Chips can have an optional icon.
  readonly icon?: string;
  // Use minimal padding, reducing the overall size of the chip by a few px.
  // Defaults to false.
  readonly compact?: boolean;
  // List of space separated class names forwarded to the icon.
  readonly className?: string;
  // Show loading spinner instead of icon.
  // Defaults to false.
  readonly loading?: boolean;
  // Whether to use a filled icon
  // Defaults to false;
  readonly iconFilled?: boolean;
  // Indicate chip colouring by intent.
  // Defaults to undefined aka "None"
  readonly intent?: Intent;
  // Turns the chip into a pill shape.
  readonly rounded?: boolean;
  // If true, shows a little cross on the right hand side.
  readonly removable?: boolean;
  // Called when the little cross is pressed (only applicable when removable is
  // true).
  readonly onRemove?: () => void;
  // Title for the remove button (only applicable when removable is true).
  readonly removeButtonTitle?: string;
}

export class Chip implements m.ClassComponent<ChipAttrs> {
  view({attrs}: m.CVnode<ChipAttrs>) {
    const {
      icon,
      compact,
      className,
      iconFilled,
      intent = Intent.None,
      rounded,
      removable,
      onRemove,
      label,
      removeButtonTitle,
      ...htmlAttrs
    } = attrs;

    const classes = classNames(
      compact && 'pf-compact',
      classForIntent(intent),
      className,
      rounded && 'pf-chip--rounded',
    );

    return m(
      '.pf-chip',
      {
        ...htmlAttrs,
        className: classes,
      },
      icon &&
        m(Icon, {
          className: 'pf-chip__icon',
          icon: icon,
          filled: iconFilled,
        }),
      m('span.pf-chip__label', label),
      removable &&
        m(Button, {
          compact: true,
          rounded,
          icon: 'close',
          title: removeButtonTitle ?? 'Remove',
          onclick: () => onRemove?.(),
        }),
    );
  }
}
