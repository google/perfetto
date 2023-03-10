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

export interface MenuItemAttrs {
  label: string;
  icon?: string;
  disabled?: boolean;
  [htmlAttrs: string]: any;
}

// An interactive menu element with an icon.
export class MenuItem implements m.ClassComponent<MenuItemAttrs> {
  view({attrs}: m.CVnode<MenuItemAttrs>) {
    const {label, icon, disabled, ...htmlAttrs} = attrs;

    return m(
        'button.pf-menu-item' + (disabled ? '[disabled]' : ''),
        htmlAttrs,
        icon && m('i.material-icons', icon),
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
