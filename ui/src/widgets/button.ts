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
import {Popup, PopupPosition} from './popup';
import {assertUnreachable} from '../base/assert';
import {isEmptyVnodes} from '../base/mithril_utils';
import {Tooltip} from './tooltip';

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
  readonly active?: boolean;
  // Use minimal padding, reducing the overall size of the button by a few px.
  // Defaults to false.
  readonly compact?: boolean;
  // Optional right icon.
  readonly rightIcon?: string;
  // List of space separated class names forwarded to the icon.
  readonly className?: string;
  // Allow clicking this button to close parent popups.
  // Defaults to false.
  readonly dismissPopup?: boolean;
  // Show loading spinner instead of icon.
  // Defaults to false.
  readonly loading?: boolean;
  // Whether to use a filled icon
  // Defaults to false;
  readonly iconFilled?: boolean;
  // Indicate the intent of the button using color.
  // Defaults to undefined aka "None"
  readonly intent?: Intent;
  // Choose what style the button should have.
  // - Filled: The button has a background - used for standalone buttons.
  // - Text: The button has no visible background - used for when many buttons
  //   appear together and styling on each one would be too visually busy e.g.
  //   on toolbars.
  // Defaults to Filled.
  readonly variant?: ButtonVariant;
  // Turns the button into a pill shape.
  readonly rounded?: boolean;
  // Makes the button shrink to fit inside it's container, rather than its width
  // being defined by its content. Useful for when you have buttons with dynamic
  // content that may change size, and you don't want the button to change size
  // as that happens. Defaults to false.
  readonly shrink?: boolean;
  // Optional tooltip to show on hover.
  readonly tooltip?: m.Children;
}

interface IconButtonAttrs extends CommonAttrs {
  // Icon buttons require an icon.
  readonly icon: string;
}

interface LabelButtonAttrs extends CommonAttrs {
  // Label buttons require a label.
  readonly label: string;
  // Label buttons can have an optional icon.
  readonly icon?: string;
}

export type ButtonAttrs = LabelButtonAttrs | IconButtonAttrs;

function isLabelButtonAttrs(attrs: ButtonAttrs): attrs is LabelButtonAttrs {
  return (attrs as LabelButtonAttrs).label !== undefined;
}

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
      rounded,
      shrink,
      loading,
      tooltip,
      ...htmlAttrs
    } = attrs;

    const label = isLabelButtonAttrs(attrs) ? attrs.label : undefined;
    const iconOnly = Boolean(icon && !label);

    const classes = classNames(
      active && 'pf-active',
      compact && 'pf-compact',
      classForVariant(variant),
      classForIntent(intent),
      iconOnly && 'pf-icon-only',
      dismissPopup && Popup.DISMISS_POPUP_GROUP_CLASS,
      rounded && 'pf-button--rounded',
      shrink && 'pf-button--shrink',
      loading && 'pf-button--loading',
      className,
    );

    const button = m(
      'button.pf-button',
      {
        ...htmlAttrs,
        className: classes,
      },
      this.renderIcon(attrs),
      m('span', {className: 'pf-button__label'}, label),
      rightIcon &&
        m(Icon, {
          className: 'pf-right-icon',
          icon: rightIcon,
          filled: iconFilled,
        }),
    );

    if (isEmptyVnodes(tooltip)) {
      // No tooltip, just render the button directly.
      return button;
    } else {
      // Wrap the button in a tooltip.
      return m(
        Tooltip,
        {
          trigger: button,
          position: PopupPosition.Top,
        },
        m('span.pf-button__tooltip', tooltip),
      );
    }
  }

  private renderIcon(attrs: ButtonAttrs): m.Children {
    const {icon, iconFilled} = attrs;
    const className = 'pf-left-icon';
    if (icon) {
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

/**
 * A set of buttons that are visually grouped together into one super-widget.
 * The inside borders are de-duplicated, and the inside rounded corners removed.
 *
 * This is useful for when you have a set of radio buttons, or a button with an
 * additional dropdown button to allow for additional actions to be selected.
 *
 * Very similar to the SegmentedButtons widget, but offers more control over the
 * individual buttons.
 */
export class ButtonGroup implements m.ClassComponent<HTMLAttrs> {
  view({attrs, children}: m.CVnode<HTMLAttrs>): m.Children {
    return m('.pf-button-group', attrs, children);
  }
}
