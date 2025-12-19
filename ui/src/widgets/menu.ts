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
import {hasChildren} from '../base/mithril_utils';
import {HTMLAttrs} from './common';
import {Icon} from './icon';
import {Popup, PopupAttrs, PopupPosition} from './popup';

export interface MenuItemAttrs extends HTMLAttrs {
  // Text to display on the menu button.
  label: m.Children;
  // Optional left icon.
  icon?: string;
  // Optional right icon.
  rightIcon?: string;
  // Make the item appear greyed out block any interaction with it. No events
  // will be fired.
  // Defaults to false.
  disabled?: boolean;
  // Always show the button as if the "active" pseudo class were applied, which
  // makes the button look permanently pressed.
  // Useful for when the button represents some toggleable state, such as
  // showing/hiding a popup menu.
  // Defaults to false.
  active?: boolean;
  // If this menu item is a descendant of a popup, this setting means that
  // clicking it will result in the popup being dismissed.
  // Defaults to false when menuitem has children, true otherwise.
  closePopupOnClick?: boolean;

  // Callback for when the menu is opened (only when the menu item has children).
  onChange?(isOpen: boolean): void;
}

// An interactive menu element with an icon.
// If this node has children, a nested popup menu will be rendered.
export class MenuItem implements m.ClassComponent<MenuItemAttrs> {
  view(vnode: m.CVnode<MenuItemAttrs>): m.Children {
    if (hasChildren(vnode)) {
      return this.renderNested(vnode);
    } else {
      return this.renderSingle(vnode);
    }
  }

  private renderNested({attrs, children}: m.CVnode<MenuItemAttrs>) {
    const {
      rightIcon = 'chevron_right',
      closePopupOnClick = false,
      onChange,
      ...rest
    } = attrs;

    return m(
      PopupMenu,
      {
        position: PopupPosition.RightStart,
        trigger: m(MenuItem, {
          rightIcon: rightIcon,
          closePopupOnClick,
          ...rest,
        }),
        onChange,
        showArrow: false,
        createNewGroup: false,
        edgeOffset: 5, // Adjust for popup padding & border.
      },
      children,
    );
  }

  private renderSingle({attrs}: m.CVnode<MenuItemAttrs>) {
    const {
      label,
      icon,
      rightIcon,
      disabled,
      active,
      closePopupOnClick = true,
      className,
      ...htmlAttrs
    } = attrs;

    const classes = classNames(
      active && 'pf-active',
      !disabled && closePopupOnClick && Popup.DISMISS_POPUP_GROUP_CLASS,
      className,
    );

    return m(
      'button.pf-menu-item' + (disabled ? '[disabled]' : ''),
      {
        ...htmlAttrs,
        className: classes,
      },
      icon && m(Icon, {className: 'pf-menu-item__left-icon', icon}),
      m('.pf-menu-item__label', label),
      rightIcon &&
        m(Icon, {className: 'pf-menu-item__right-icon', icon: rightIcon}),
    );
  }
}

// An element which shows a dividing line between menu items.
export class MenuDivider implements m.ClassComponent {
  view() {
    return m('.pf-menu-divider');
  }
}

export interface MenuTitleAttrs extends HTMLAttrs {
  // Text to display in the title.
  readonly label?: string;
}

// An element which shows a dividing line between menu items.
export class MenuTitle implements m.ClassComponent {
  view({attrs}: m.CVnode<MenuTitleAttrs>) {
    return m('.pf-menu-title', attrs.label);
  }
}

// A siple container for a menu.
// The menu contents are passed in as children, and are typically MenuItems or
// MenuDividers, but really they can be any Mithril component.
export class Menu implements m.ClassComponent<HTMLAttrs> {
  view({attrs, children}: m.CVnode<HTMLAttrs>) {
    return m('.pf-menu', attrs, children);
  }
}

interface PopupMenuAttrs extends PopupAttrs {
  // Whether this popup should form a new popup group.
  // When nesting popups, grouping controls how popups are closed.
  // When closing popups via the Escape key, each group is closed one by one,
  // starting at the topmost group in the stack.
  // When using a magic button to close groups (see DISMISS_POPUP_GROUP_CLASS),
  // only the group in which the button lives and it's children will be closed.
  // Defaults to true.
  createNewGroup?: boolean;
}

// A combination of a Popup and a Menu component.
// The menu contents are passed in as children, and are typically MenuItems or
// MenuDividers, but really they can be any Mithril component.
export class PopupMenu implements m.ClassComponent<PopupMenuAttrs> {
  view({attrs, children}: m.CVnode<PopupMenuAttrs>) {
    const {trigger, position = PopupPosition.Bottom, ...popupAttrs} = attrs;

    return m(
      Popup,
      {
        trigger,
        position,
        className: 'pf-popup-menu',
        ...popupAttrs,
      },
      m(Menu, children),
    );
  }
}
