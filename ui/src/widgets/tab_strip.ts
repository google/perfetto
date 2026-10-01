// Copyright (C) 2026 The Android Open Source Project
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

import './tab_strip.scss';
import m from 'mithril';
import {classNames} from '../base/classnames';
import {Icons} from '../base/semantic_icons';
import {Button} from './button';
import type {HTMLAnchorAttrs, HTMLAttrs} from './common';
import {Icon} from './icon';
import {PopupMenu} from './menu';
import {PopupPosition} from './popup';

// A composable tab bar: just the bar and its styling. Put whatever you like
// inside it - TabStrip.Tab (a clickable tab), buttons, or arbitrary content.
//
// The bar does not track which tab is active: every item says whether it is
// active itself. This makes it trivial to drive from a router (derive the
// active item from the URL) or from any other state.
//
//   m(TabStrip, {variant: 'underline', rightContent: m(Button, {...})},
//     m(TabStrip.Tab, {active: page === 'foo', onclick: () => ...}, 'Foo'),
//     m(TabStrip.Tab, {active: page === 'bar', onclick: () => ...}, 'Bar'),
//   );
//
// For a batteries-included tabs component that also manages tab content and
// active state, see Tabs.
export interface TabStripAttrs extends HTMLAttrs {
  // Visual style of the bar. 'card' (the default) renders boxed tab handles
  // on a secondary-background bar; 'underline' renders flat text tabs with a
  // primary underline on the active tab. Items pick up the style via CSS, so
  // they don't need to be told about it.
  readonly variant?: 'card' | 'underline';
  // Content pinned to the left of the tabs.
  readonly leftContent?: m.Children;
  // Content pinned to the right of the bar.
  readonly rightContent?: m.Children;
}

export class TabStrip implements m.ClassComponent<TabStripAttrs> {
  view({attrs, children}: m.CVnode<TabStripAttrs>): m.Children {
    const {
      variant = 'card',
      leftContent,
      rightContent,
      className,
      ...htmlAttrs
    } = attrs;
    return m(
      '.pf-tab-strip',
      {
        ...htmlAttrs,
        className: classNames(
          className,
          variant === 'underline' && 'pf-tab-strip--underline',
        ),
      },
      leftContent !== undefined && m('.pf-tab-strip__left', leftContent),
      m('.pf-tab-strip__tabs', children),
      rightContent !== undefined && m('.pf-tab-strip__right', rightContent),
    );
  }
}

export namespace TabStrip {
  export interface TabAttrs extends HTMLAttrs {
    // Whether this tab is the active one.
    readonly active?: boolean;
    // Whether this tab is disabled: greyed out, and clicks and close requests
    // are ignored.
    readonly disabled?: boolean;
    // Icon shown before the title: an icon name, or arbitrary content.
    readonly icon?: string | m.Children;
    // Called when the close button is clicked. When set, a close button is
    // shown and middle-clicking the tab also closes it.
    readonly onClose?: () => void;
    // Menu items to show in a dropdown on the tab. When set, a menu button
    // appears when the tab is hovered.
    readonly menuItems?: m.Children;
    // Called when the tab's title is renamed via inline editing. When set,
    // double-clicking the tab turns its title into a text input, pre-filled
    // with the title's text. Enter or blur commits; Escape or an empty
    // (after trim) value cancels, in which case this is not called.
    readonly onRename?: (newTitle: string) => void;
  }

  // A clickable tab for use inside TabStrip.
  export class Tab implements m.ClassComponent<TabAttrs> {
    private renaming = false;
    private renameValue = '';

    view({attrs, children}: m.CVnode<TabAttrs>): m.Children {
      const {
        active,
        disabled,
        icon,
        onClose,
        menuItems,
        onRename,
        onclick,
        ondblclick,
        className,
        ...htmlAttrs
      } = attrs;
      return m(
        '.pf-tab-strip__tab',
        {
          ...htmlAttrs,
          'className': classNames(
            className,
            active && 'pf-tab-strip__tab--active',
            disabled && 'pf-tab-strip__tab--disabled',
          ),
          'aria-disabled': disabled ? 'true' : undefined,
          'onclick': disabled ? undefined : onclick,
          'ondblclick': (e: PointerEvent) => {
            ondblclick?.(e);
            if (onRename && !disabled) {
              const target = e.currentTarget as HTMLElement;
              const title = target.querySelector('.pf-tab-strip__tab-title');
              this.renameValue = title?.textContent ?? '';
              this.renaming = true;
            }
          },
          'onauxclick': !disabled && onClose ? () => onClose() : undefined,
        },
        this.renaming && onRename
          ? [renderTabIcon(icon), this.renderRenameInput(onRename)]
          : renderTabContent(icon, children),
        menuItems !== undefined &&
          m(
            PopupMenu,
            {
              trigger: m(Button, {
                compact: true,
                icon: Icons.ContextMenuAlt,
                className: 'pf-tab-strip__tab-menu-btn',
                disabled,
              }),
              position: PopupPosition.Bottom,
            },
            menuItems,
          ),
        onClose &&
          m(Button, {
            compact: true,
            icon: Icons.Close,
            disabled,
            onclick: (e: Event) => {
              e.stopPropagation();
              onClose();
            },
          }),
      );
    }

    private renderRenameInput(onRename: (newTitle: string) => void) {
      const commit = () => {
        // Guard against the blur fired when the input is removed after Enter
        // or Escape has already ended the rename.
        if (!this.renaming) return;
        this.renaming = false;
        const newTitle = this.renameValue.trim();
        if (newTitle) {
          onRename(newTitle);
        }
      };
      return m('input.pf-tab-strip__tab-rename-input', {
        value: this.renameValue,
        oncreate: (vnode: m.VnodeDOM) => {
          const el = vnode.dom as HTMLInputElement;
          el.focus();
          el.select();
        },
        oninput: (e: InputEvent) => {
          this.renameValue = (e.target as HTMLInputElement).value;
        },
        onkeydown: (e: KeyboardEvent) => {
          if (e.key === 'Enter') {
            commit();
            e.preventDefault();
          } else if (e.key === 'Escape') {
            this.renaming = false;
            e.preventDefault();
          }
          e.stopPropagation();
        },
        onblur: commit,
        onclick: (e: Event) => e.stopPropagation(),
      });
    }
  }

  export interface LinkAttrs extends HTMLAnchorAttrs {
    // Whether this tab is the active one (typically derived from the URL).
    readonly active?: boolean;
    // Whether this tab is disabled: greyed out, and the link can't be followed.
    readonly disabled?: boolean;
    // Icon shown before the title: an icon name, or arbitrary content.
    readonly icon?: string | m.Children;
    // Called when the close button is clicked. When set, a close button is
    // shown. Unlike Tab, middle-click is left to the browser (open in new tab)
    // rather than closing the tab.
    readonly onClose?: () => void;
  }

  // A tab that is a real link, for use inside TabStrip. Navigation is left to
  // the browser, so right-click, middle-click, open-in-new-tab and history all
  // work as for any other link. Unlike Tab, links can't be renamed.
  export class Link implements m.ClassComponent<LinkAttrs> {
    view({attrs, children}: m.CVnode<LinkAttrs>): m.Children {
      const {
        active,
        disabled,
        icon,
        href,
        onclick,
        onClose,
        className,
        ...htmlAttrs
      } = attrs;
      const preventNav = active || disabled;
      return m(
        'a.pf-tab-strip__tab',
        {
          ...htmlAttrs,
          'className': classNames(
            className,
            active && 'pf-tab-strip__tab--active',
            disabled && 'pf-tab-strip__tab--disabled',
          ),
          'aria-current': active ? 'page' : undefined,
          'aria-disabled': disabled ? 'true' : undefined,
          // An <a> without an href can't be followed (click, middle-click, or
          // open in new tab), which is exactly what disabled should mean.
          'href': preventNav ? undefined : href,
          'onclick': preventNav ? undefined : onclick,
        },
        renderTabContent(icon, children),
        onClose &&
          m(Button, {
            compact: true,
            icon: Icons.Close,
            disabled,
            onclick: (e: Event) => {
              // Don't follow the enclosing link.
              e.preventDefault();
              e.stopPropagation();
              onClose();
            },
          }),
      );
    }
  }
}

function renderTabContent(
  icon: string | m.Children,
  children: m.Children,
): m.Children {
  return [renderTabIcon(icon), m('.pf-tab-strip__tab-title', children)];
}

function renderTabIcon(icon: string | m.Children): m.Children {
  if (icon === undefined || icon === null) {
    return undefined;
  }
  if (typeof icon === 'string') {
    return m(Icon, {icon, className: 'pf-tab-strip__tab-icon'});
  }
  return m('.pf-tab-strip__tab-icon', icon);
}
