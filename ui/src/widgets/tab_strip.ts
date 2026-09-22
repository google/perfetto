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
import {Icon} from './icon';

export interface TabStripAttrs {
  // Additional class name for the container.
  readonly className?: string;
}

export interface TabStripTabAttrs {
  // Whether this tab is currently active.
  readonly active?: boolean;
  // Called when the tab is clicked.
  readonly onclick?: () => void;
  // Additional class name for the tab.
  readonly className?: string;
  // Icon to display on the left side of the tab title.
  readonly leftIcon?: string | m.Children;
  // Icon to display on the right side of the tab title.
  readonly rightIcon?: string | m.Children;
}

class Tab implements m.ClassComponent<TabStripTabAttrs> {
  view({attrs, children}: m.CVnode<TabStripTabAttrs>): m.Children {
    const {active, onclick, className, leftIcon, rightIcon} = attrs;
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
      {active, onclick, className},
      [
        renderIcon(leftIcon, 'pf-tab-strip__tab-icon--left'),
        m('span.pf-tab-strip__tab-title', children),
        renderIcon(rightIcon, 'pf-tab-strip__tab-icon--right'),
      ],
    );
  }
}

/**
 * A horizontal tab navigation component. Tabs are passed as children using
 * the `TabStrip.Tab` sub-component:
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
    return m(
      '.pf-tab-strip',
      {className: attrs.className},
      m('.pf-tab-strip__tabs', children),
    );
  }
}
