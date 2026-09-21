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
import {Anchor} from './anchor';
import {Icon} from './icon';
import type {Tabs, TabsAttrs, TabsTab} from './tabs';

/** @deprecated Use {@link TabsTab} from `./tabs` instead. */
export interface TabOption {
  readonly key: string;
  readonly title: string;
  // When set, the tab handle is rendered as an Anchor to this URL (so it can
  // be middle-clicked / opened in a new tab) instead of a plain div. Clicks
  // navigate via the href; onTabChange is not fired.
  readonly href?: string;
  readonly leftIcon?: string | m.Children;
  readonly rightIcon?: string | m.Children;
}

/** @deprecated Use {@link TabsAttrs} from `./tabs` instead. */
export interface TabStripAttrs {
  readonly className?: string;
  readonly tabs: ReadonlyArray<TabOption>;
  readonly currentTabKey: string;
  readonly onTabChange: (key: string) => void;
}

/** @deprecated Use {@link Tabs} from `./tabs` instead. */
export class TabStrip implements m.ClassComponent<TabStripAttrs> {
  view({attrs}: m.CVnode<TabStripAttrs>) {
    const {tabs, currentTabKey, onTabChange, className} = attrs;
    return m(
      '.pf-tab-strip',
      {className},
      m(
        '.pf-tab-strip__tabs',
        tabs.map((tab) => {
          const {key, title, leftIcon, rightIcon, href} = tab;
          const renderIcon = (
            icon: string | m.Children | undefined,
            className: string,
          ) => {
            if (icon === undefined) {
              return undefined;
            }
            if (typeof icon === 'string') {
              return m(Icon, {icon, className});
            }
            return m('.pf-tab-strip__tab-icon', {className}, icon);
          };
          const children = [
            renderIcon(leftIcon, 'pf-tab-strip__tab-icon--left'),
            m('span.pf-tab-strip__tab-title', title),
            renderIcon(rightIcon, 'pf-tab-strip__tab-icon--right'),
          ];
          if (href !== undefined) {
            return m(
              Anchor,
              {
                class: 'pf-tab-strip__tab',
                active: currentTabKey === key,
                key,
                href,
              },
              children,
            );
          }
          return m(
            '.pf-tab-strip__tab',
            {
              active: currentTabKey === key,
              key,
              onclick: () => {
                onTabChange(key);
              },
            },
            children,
          );
        }),
      ),
    );
  }
}
