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

import * as m from 'mithril';
import {classNames} from '../classnames';
import {Icon} from './icon';
import {ActivationMode, Popup, PopupPosition} from './popup';
import {hasChildren} from './utils';

export interface MenuItemAttrs {
  label: string;
  icon?: string;
  rightIcon?: string;
  disabled?: boolean;
  active?: boolean;
  [htmlAttrs: string]: any;
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
    const {rightIcon = 'chevron_right', ...rest} = attrs;

    return m(
        PopupMenu2,
        {
          popupPosition: PopupPosition.RightStart,
          trigger: m(MenuItem, {
            rightIcon: rightIcon ?? 'chevron_right',
            ...rest,
          }),
          activationMode: ActivationMode.Hover,
          showArrow: false,
        },
        children,
    );
  }

  private renderSingle({attrs}: m.CVnode<MenuItemAttrs>) {
    const {label, icon, rightIcon, disabled, active, ...htmlAttrs} = attrs;

    const classes = classNames(
        active && 'pf-active',
    );

    return m(
        'button.pf-menu-item' + (disabled ? '[disabled]' : ''),
        {class: classes, ...htmlAttrs},
        icon && m(Icon, {className: 'pf-left-icon', icon}),
        rightIcon && m(Icon, {className: 'pf-right-icon', icon: rightIcon}),
        label,
    );
  }
};

// An element which shows a dividing line between menu items.
export class MenuDivider implements m.ClassComponent {
  view() {
    return m('.pf-menu-divider');
  }
};

// A siple container for a menu.
// The menu contents are passed in as children, and are typically MenuItems or
// MenuDividers, but really they can be any Mithril component.
export class Menu implements m.ClassComponent {
  view({children}: m.CVnode) {
    return m('.pf-menu', children);
  }
};

interface PopupMenu2Attrs {
  // The trigger is mithril component which is used to toggle the popup when
  // clicked, and provides the anchor on the page which the popup shall hover
  // next to, and to which the popup's arrow shall point. The popup shall move
  // around the page with this component, as if attached to it.
  // This trigger can be any mithril component, but it is typically a Button,
  // an Icon, or some other interactive component.
  // Beware this element will have its `onclick`, `ref`, and `active` attributes
  // overwritten.
  trigger: m.Vnode<any, any>;
  // Which side of the trigger to place to popup.
  // Defaults to "bottom".
  popupPosition?: PopupPosition;
  // How the popup is opened.
  // Defaults to "click".
  activationMode?: ActivationMode;
  // Whether we should show the little arrow pointing to the trigger.
  // Defaults to true.
  showArrow?: boolean;
}

// A combination of a Popup and a Menu component.
// The menu contents are passed in as children, and are typically MenuItems or
// MenuDividers, but really they can be any Mithril component.
export class PopupMenu2 implements m.ClassComponent<PopupMenu2Attrs> {
  view({attrs, children}: m.CVnode<PopupMenu2Attrs>) {
    const {trigger, popupPosition = PopupPosition.Bottom, ...popupAttrs} =
        attrs;

    return m(
        Popup,
        {
          trigger,
          position: popupPosition,
          closeOnContentClick: true,
          ...popupAttrs,
        },
        m(Menu, children));
  }
};
