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

import './tab_strip.scss';
import m from 'mithril';
import {classNames} from '../base/classnames';
import {Button} from './button';
import {Icon} from './icon';
import {Icons} from '../base/semantic_icons';
import {PopupMenu} from './menu';
import {PopupPosition} from './popup';

export interface TabStripAttrs {
  // Additional class name for the container.
  readonly className?: string;
  // Visual style of the tab bar. 'card' (the default) renders classic
  // boxed tab handles on a secondary-background bar; 'underline' renders
  // flat text tabs with a primary underline on the active tab.
  readonly variant?: 'card' | 'underline';
}

export interface TabStripTabAttrs {
  // Whether this tab is currently active.
  readonly active?: boolean;
  // Called when the tab is clicked.
  readonly onclick?: () => void;
  // Called when a pointer is pressed down on the tab.
  readonly onpointerdown?: () => void;
  // Called when the tab is double-clicked.
  readonly ondblclick?: () => void;
  // Additional class name for the tab.
  readonly className?: string;
  // Icon to display on the left side of the tab title.
  readonly leftIcon?: string | m.Children;
  // Icon to display on the right side of the tab title.
  readonly rightIcon?: string | m.Children;
  // Whether to show a close button on the tab.
  readonly closeButton?: boolean;
  // Called when the tab's close button is clicked.
  readonly onClose?: () => void;
  // Optional menu items to show in a dropdown menu on the tab.
  // When provided, a menu button appears on hover.
  readonly menuItems?: m.Children;
  // Whether the tab title is currently being edited via inline renaming.
  readonly renaming?: boolean;
  // The current value of the rename input.
  readonly renameValue?: string;
  // Called as the user types in the rename input.
  readonly onRenameInput?: (value: string) => void;
  // Called when the rename is committed (Enter or blur).
  readonly onRenameCommit?: () => void;
  // Called when the rename is cancelled (Escape).
  readonly onRenameCancel?: () => void;
  // Whether the tab can be dragged (e.g. for reordering).
  readonly draggable?: boolean;
  readonly ondragstart?: (e: DragEvent) => void;
  readonly ondragend?: (e: DragEvent) => void;
  readonly ondragover?: (e: DragEvent) => void;
  readonly ondragleave?: (e: DragEvent) => void;
  readonly ondrop?: (e: DragEvent) => void;
}

class Tab implements m.ClassComponent<TabStripTabAttrs> {
  view({attrs, children}: m.CVnode<TabStripTabAttrs>): m.Children {
    const {
      active,
      onclick,
      onpointerdown,
      ondblclick,
      className,
      leftIcon,
      rightIcon,
      closeButton,
      onClose,
      menuItems,
      renaming,
      renameValue,
      onRenameInput,
      onRenameCommit,
      onRenameCancel,
      draggable,
      ondragstart,
      ondragend,
      ondragover,
      ondragleave,
      ondrop,
    } = attrs;

    const renderIcon = (
      icon: string | m.Children | undefined,
      iconClassName: string,
    ) => {
      if (icon === undefined) {
        return undefined;
      }
      if (typeof icon === 'string') {
        return m(Icon, {icon, className: iconClassName});
      }
      return m('.pf-tab-strip__tab-icon', {className: iconClassName}, icon);
    };

    return m(
      '.pf-tab-strip__tab',
      {
        className: classNames(className, active && 'pf-tab-strip__tab--active'),
        onclick,
        onpointerdown,
        ondblclick,
        onauxclick: () => onClose?.(),
        draggable,
        ondragstart,
        ondragend,
        ondragover,
        ondragleave,
        ondrop,
      },
      [
        renderIcon(leftIcon, 'pf-tab-strip__tab-icon--left'),
        renaming
          ? m('input.pf-tab-strip__tab-rename-input', {
              value: renameValue,
              oncreate: (vnode: m.VnodeDOM) => {
                const el = vnode.dom as HTMLInputElement;
                el.focus();
                el.select();
              },
              oninput: (e: InputEvent) => {
                const target = e.target as HTMLInputElement;
                onRenameInput?.(target.value);
              },
              onkeydown: (e: KeyboardEvent) => {
                if (e.key === 'Enter') {
                  onRenameCommit?.();
                  e.preventDefault();
                } else if (e.key === 'Escape') {
                  onRenameCancel?.();
                  e.preventDefault();
                }
                e.stopPropagation();
              },
              onblur: () => onRenameCommit?.(),
              onclick: (e: Event) => e.stopPropagation(),
            })
          : m('span.pf-tab-strip__tab-title', children),
        renderIcon(rightIcon, 'pf-tab-strip__tab-icon--right'),
        menuItems !== undefined &&
          m(
            PopupMenu,
            {
              trigger: m(Button, {
                compact: true,
                icon: Icons.ContextMenuAlt,
                className: 'pf-tab-strip__tab-menu-btn',
              }),
              position: PopupPosition.Bottom,
            },
            menuItems,
          ),
        closeButton &&
          m(Button, {
            compact: true,
            icon: Icons.Close,
            onclick: (e: Event) => {
              e.stopPropagation();
              onClose?.();
            },
          }),
      ],
    );
  }
}

/**
 * A horizontal tab bar. Tabs are passed as children using the
 * `TabStrip.Tab` sub-component:
 *
 * ```ts
 * m(
 *   TabStrip,
 *   m(TabStrip.Tab, {active: true, onclick: () => {}}, 'Content'),
 *   m(TabStrip.Tab, {active: false, onclick: () => {}}, 'Other'),
 * );
 * ```
 */
export class TabStrip implements m.ClassComponent<TabStripAttrs> {
  static readonly Tab = Tab;

  view({attrs, children}: m.CVnode<TabStripAttrs>): m.Children {
    const {className, variant = 'card'} = attrs;
    return m(
      '.pf-tab-strip',
      {
        className: classNames(
          className,
          variant === 'underline' && 'pf-tab-strip--underline',
        ),
      },
      m('.pf-tab-strip__tabs', children),
    );
  }
}
