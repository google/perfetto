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

import m from 'mithril';
import {classNames} from '../base/classnames';
import {Gate} from '../base/mithril_utils';
import {Button} from './button';
import {Icon} from './icon';
import {Icons} from '../base/semantic_icons';

export interface TabsTab {
  // Unique identifier for the tab.
  readonly key: string;
  // Content to display in the tab handle.
  readonly title: m.Children;
  // Content to display when this tab is active.
  readonly content: m.Children;
  // Whether to show a close button on the tab.
  readonly closeButton?: boolean;
  // Icon to display on the left side of the tab title.
  readonly leftIcon?: string | m.Children;
}

export interface TabsParts {
  // The tab handle buttons.
  readonly handles: m.Children;
  // The tab content panels (with Gate for show/hide).
  readonly content: m.Children;
}

export interface TabsAttrs {
  // The tabs to display.
  readonly tabs: TabsTab[];
  // The currently active tab key (controlled mode).
  // If not provided, the component manages its own state (uncontrolled mode).
  readonly activeTabKey?: string;
  // Called when a tab is clicked.
  onTabChange?(key: string): void;
  // Called when a tab's close button is clicked.
  onTabClose?(key: string): void;
  // Additional class name for the container (only used in default layout).
  readonly className?: string;
  // Optional custom render function. When provided, Tabs becomes "headless" -
  // it manages tab state but you control where handles and content are placed.
  // If not provided, uses the default .pf-tabs layout.
  render?(parts: TabsParts): m.Children;
}

interface TabHandleAttrs {
  readonly active?: boolean;
  readonly hasCloseButton?: boolean;
  readonly onClose?: () => void;
  readonly onclick?: () => void;
  readonly leftIcon?: string | m.Children;
}

class TabHandle implements m.ClassComponent<TabHandleAttrs> {
  view({attrs, children}: m.CVnode<TabHandleAttrs>): m.Children {
    const {active, hasCloseButton, onClose, onclick, leftIcon} = attrs;

    const renderLeftIcon = () => {
      if (leftIcon === undefined) {
        return undefined;
      }
      const style = {alignSelf: 'center'};
      if (typeof leftIcon === 'string') {
        return m(Icon, {icon: leftIcon, className: 'pf-tabs__tab-icon', style});
      }
      return m('.pf-tabs__tab-icon', {style}, leftIcon);
    };

    return m(
      '.pf-tabs__tab',
      {
        className: classNames(active && 'pf-tabs__tab--active'),
        onclick,
        onauxclick: () => onClose?.(),
      },
      renderLeftIcon(),
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
    const {tabs, activeTabKey, onTabChange, onTabClose, className, render} =
      attrs;

    // Get active tab key (controlled or uncontrolled)
    const activeKey = activeTabKey ?? this.internalActiveTab ?? tabs[0]?.key;

    // Build the tab handles
    const handles = tabs.map((tab) =>
      m(
        TabHandle,
        {
          active: tab.key === activeKey,
          hasCloseButton: tab.closeButton,
          leftIcon: tab.leftIcon,
          onclick: () => {
            this.internalActiveTab = tab.key;
            onTabChange?.(tab.key);
          },
          onClose: () => onTabClose?.(tab.key),
        },
        tab.title,
      ),
    );

    // Build the tab content
    const content = tabs.map((tab) =>
      m(Gate, {key: tab.key, open: tab.key === activeKey}, tab.content),
    );

    // If custom render provided, let caller control layout
    if (render) {
      return render({handles, content});
    }

    // Default layout
    return m(
      '.pf-tabs',
      {className},
      m('.pf-tabs__tabs', handles),
      m('.pf-tabs__content', content),
    );
  }
}
