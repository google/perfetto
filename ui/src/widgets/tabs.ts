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

import m from 'mithril';
import {classNames} from '../base/classnames';
import {Gate} from '../base/mithril_utils';
import {Button} from './button';
import {Icon} from './icon';
import {Icons} from '../base/semantic_icons';

export interface TabBarTab {
  // Unique identifier for the tab.
  readonly key: string;
  // Content to display in the tab handle.
  readonly title: m.Children;
  // Content to display when this tab is active.
  readonly content: m.Children;
  // Optional icon to display before the title.
  readonly icon?: string;
  // Whether to show a close button on the tab.
  readonly closable?: boolean;
}

export interface TabsAttrs {
  // The tabs to display.
  readonly tabs: TabBarTab[];
  // The currently active tab key (controlled mode).
  // If not provided, the component manages its own state (uncontrolled mode).
  readonly activeTabKey?: string;
  // Called when a tab is clicked.
  onTabChange?(key: string): void;
  // Called when a tab's close button is clicked.
  onTabClose?(key: string): void;
  // Called when the add tab button is clicked.
  // If provided, an add tab button will be shown.
  onAddTab?(): void;
  // Additional class name for the container.
  readonly className?: string;
}

interface TabHandleAttrs {
  readonly active?: boolean;
  readonly icon?: string;
  readonly hasCloseButton?: boolean;
  readonly onClose?: () => void;
  readonly onclick?: () => void;
}

class TabHandle implements m.ClassComponent<TabHandleAttrs> {
  view({attrs, children}: m.CVnode<TabHandleAttrs>): m.Children {
    const {active, icon, hasCloseButton, onClose, onclick} = attrs;
    return m(
      '.pf-tabs__tab',
      {
        className: classNames(active && 'pf-tabs__tab--active'),
        onclick,
        onauxclick: () => onClose?.(),
      },
      icon && m(Icon, {icon}),
      m('.pf-tabs__tab-title', children),
      hasCloseButton &&
        m(Button, {
          compact: true,
          icon: Icons.Close,
          onclick: (e: Event) => {
            e.stopPropagation();
            onClose?.();
          },
        }),
    );
  }
}

export class Tabs implements m.ClassComponent<TabsAttrs> {
  // Current active tab key (for uncontrolled mode).
  private internalActiveTab?: string;

  view({attrs}: m.CVnode<TabsAttrs>): m.Children {
    const {tabs, activeTabKey, onTabChange, onTabClose, onAddTab, className} =
      attrs;

    if (tabs.length === 0) {
      return null;
    }

    // Get active tab key (controlled or uncontrolled)
    const activeKey = activeTabKey ?? this.internalActiveTab ?? tabs[0].key;

    return m(
      '.pf-tabs',
      {className},
      m(
        '.pf-tabs__tabs',
        tabs.map((tab) =>
          m(
            TabHandle,
            {
              active: tab.key === activeKey,
              icon: tab.icon,
              hasCloseButton: tab.closable,
              onclick: () => {
                this.internalActiveTab = tab.key;
                onTabChange?.(tab.key);
              },
              onClose: () => onTabClose?.(tab.key),
            },
            tab.title,
          ),
        ),
        onAddTab &&
          m(Button, {
            className: 'pf-tabs__add-tab',
            compact: true,
            icon: Icons.Add,
            title: 'Add new tab',
            onclick: onAddTab,
          }),
      ),
      m(
        '.pf-tabs__content',
        tabs.map((tab) => m(Gate, {open: tab.key === activeKey}, tab.content)),
      ),
    );
  }
}
