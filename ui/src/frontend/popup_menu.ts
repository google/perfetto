// Copyright (C) 2022 The Android Open Source Project
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
import {globals} from './globals';

export interface RegularPopupMenuItem {
  itemType: 'regular';
  // Display text
  text: string;
  // Action on menu item click
  callback: () => void;
}

// Helper function for simplifying defining menus.
export function menuItem(
    text: string, action: () => void): RegularPopupMenuItem {
  return {
    itemType: 'regular',
    text,
    callback: action,
  };
}

export interface GroupPopupMenuItem {
  itemType: 'group';
  text: string;
  itemId: string;
  children: PopupMenuItem[];
}

export type PopupMenuItem = RegularPopupMenuItem|GroupPopupMenuItem;

interface PopupMenuButtonAttrs {
  // Icon for button opening a menu
  icon: string;
  // List of popup menu items
  items: PopupMenuItem[];
}

// To ensure having at most one popup menu on the screen at a time, we need to
// listen to click events on the whole page and close currently opened popup, if
// there's any. This class, used as a singleton, does exactly that.
class PopupHolder {
  // Invariant: global listener should be register if and only if this.popup is
  // not undefined.
  popup: PopupMenuButton|undefined = undefined;
  initialized = false;
  listener: (e: MouseEvent) => void;

  constructor() {
    this.listener = (e: MouseEvent) => {
      // Only handle those events that are not part of dropdown menu themselves.
      const hasDropdown =
          e.composedPath().find(PopupHolder.isDropdownElement) !== undefined;
      if (!hasDropdown) {
        this.ensureHidden();
      }
    };
  }

  static isDropdownElement(target: EventTarget) {
    if (target instanceof HTMLElement) {
      return target.tagName === 'DIV' && target.classList.contains('dropdown');
    }
    return false;
  }

  ensureHidden() {
    if (this.popup !== undefined) {
      this.popup.setVisible(false);
    }
  }

  clear() {
    if (this.popup !== undefined) {
      this.popup = undefined;
      window.removeEventListener('click', this.listener);
    }
  }

  showPopup(popup: PopupMenuButton) {
    this.ensureHidden();
    this.popup = popup;
    window.addEventListener('click', this.listener);
  }
}

// Singleton instance of PopupHolder
const popupHolder = new PopupHolder();

// Component that displays a button that shows a popup menu on click.
export class PopupMenuButton implements m.ClassComponent<PopupMenuButtonAttrs> {
  popupShown = false;
  expandedGroups: Set<string> = new Set();

  setVisible(visible: boolean) {
    this.popupShown = visible;
    if (this.popupShown) {
      popupHolder.showPopup(this);
    } else {
      popupHolder.clear();
    }
    globals.rafScheduler.scheduleFullRedraw();
  }

  renderItem(item: PopupMenuItem): m.Child {
    switch (item.itemType) {
      case 'regular':
        return m(
            'button.open-menu',
            {
              onclick: () => {
                item.callback();
                // Hide the menu item after the action has been invoked
                this.setVisible(false);
              },
            },
            item.text);
      case 'group':
        const isExpanded = this.expandedGroups.has(item.itemId);
        return m(
            'div',
            m('button.open-menu.disallow-selection',
              {
                onclick: () => {
                  if (this.expandedGroups.has(item.itemId)) {
                    this.expandedGroups.delete(item.itemId);
                  } else {
                    this.expandedGroups.add(item.itemId);
                  }
                  globals.rafScheduler.scheduleFullRedraw();
                },
              },
              // Show text with up/down arrow, depending on expanded state.
              item.text + (isExpanded ? ' \u25B2' : ' \u25BC')),
            isExpanded ? m('div.nested-menu',
                           item.children.map((item) => this.renderItem(item))) :
                         null);
    }
  }

  view(vnode: m.Vnode<PopupMenuButtonAttrs, this>) {
    return m(
        '.dropdown',
        m('i.material-icons',
          {
            onclick: () => {
              this.setVisible(!this.popupShown);
            },
          },
          vnode.attrs.icon),
        m(this.popupShown ? '.popup-menu.opened' : '.popup-menu.closed',
          vnode.attrs.items.map((item) => this.renderItem(item))));
  }
}
