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
import {HTMLAttrs, HTMLButtonAttrs, Intent, classForIntent} from './common';
import {Icon} from './icon';
import {Popup} from './popup';
import {Spinner} from './spinner';
import {assertUnreachable} from '../base/logging';

export enum ButtonVariant {
  Filled = 'Filled',
  Outlined = 'Outlined',
  Minimal = 'Minimal',
}

interface CommonAttrs extends HTMLButtonAttrs {
  // Always show the button as if the "active" pseudo class were applied, which
  // makes the button look permanently pressed.
  // Useful for when the button represents some toggleable state, such as
  // showing/hiding a popup menu.
  // Defaults to false.
  active?: boolean;
  // Use minimal padding, reducing the overall size of the button by a few px.
  // Defaults to false.
  compact?: boolean;
  // Optional right icon.
  rightIcon?: string;
  // List of space separated class names forwarded to the icon.
  className?: string;
  // Allow clicking this button to close parent popups.
  // Defaults to false.
  dismissPopup?: boolean;
  // Show loading spinner instead of icon.
  // Defaults to false.
  loading?: boolean;
  // Whether to use a filled icon
  // Defaults to false;
  iconFilled?: boolean;
  // Indicate the intent of the button using color.
  // Defaults to undefined aka "None"
  intent?: Intent;
  // Choose what style the button should have.
  // - Filled: The button has a background - used for standalone buttons.
  // - Text: The button has no visible background - used for when many buttons
  //   appear together and styling on each one would be too visually busy e.g.
  //   on toolbars.
  // Defaults to Filled.
  variant?: ButtonVariant;
}

interface IconButtonAttrs extends CommonAttrs {
  // Icon buttons require an icon.
  icon: string;
}

interface LabelButtonAttrs extends CommonAttrs {
  // Label buttons require a label.
  label: string;
  // Label buttons can have an optional icon.
  icon?: string;
}

export type ButtonAttrs = LabelButtonAttrs | IconButtonAttrs;

export class Button implements m.ClassComponent<ButtonAttrs> {
  view({attrs}: m.CVnode<ButtonAttrs>) {
    const {
      icon,
      active,
      compact,
      rightIcon,
      className,
      dismissPopup,
      iconFilled,
      intent = Intent.None,
      variant = ButtonVariant.Minimal,
      ...htmlAttrs
    } = attrs;

    const label = 'label' in attrs ? attrs.label : undefined;
    const iconOnly = Boolean(icon && !label);

    const classes = classNames(
      active && 'pf-active',
      compact && 'pf-compact',
      classForVariant(variant),
      classForIntent(intent),
      iconOnly && 'pf-icon-only',
      dismissPopup && Popup.DISMISS_POPUP_GROUP_CLASS,
      className,
    );

    return m(
      'button.pf-button',
      {
        ...htmlAttrs,
        className: classes,
      },
      this.renderIcon(attrs),
      label || '\u200B', // Zero width space keeps button in-flow
      rightIcon &&
        m(Icon, {
          className: 'pf-right-icon',
          icon: rightIcon,
          filled: iconFilled,
        }),
    );
  }

  private renderIcon(attrs: ButtonAttrs): m.Children {
    const {icon, iconFilled} = attrs;
    const className = 'pf-left-icon';
    if (attrs.loading) {
      return m(Spinner, {className});
    } else if (icon) {
      return m(Icon, {className, icon, filled: iconFilled});
    } else {
      return undefined;
    }
  }
}

function classForVariant(variant: ButtonVariant) {
  switch (variant) {
    case ButtonVariant.Filled:
      return 'pf-button--filled';
    case ButtonVariant.Outlined:
      return 'pf-button--outlined';
    case ButtonVariant.Minimal:
      return 'pf-button--minimal';
    default:
      assertUnreachable(variant);
  }
}

/**
 * Space buttons out with a little gap between each one.
 */
export class ButtonBar implements m.ClassComponent<HTMLAttrs> {
  view({attrs, children}: m.CVnode<HTMLAttrs>): m.Children {
    return m('.pf-button-bar', attrs, children);
  }
}
