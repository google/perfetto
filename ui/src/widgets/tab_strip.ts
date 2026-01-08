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
import {Icon} from './icon';

export interface TabOption {
  readonly key: string;
  readonly title: string;
  readonly leftIcon?: string | m.Children;
  readonly rightIcon?: string | m.Children;
}

export interface TabStripAttrs {
  readonly className?: string;
  readonly tabs: ReadonlyArray<TabOption>;
  readonly currentTabKey: string;
  onTabChange(key: string): void;
}

export class TabStrip implements m.ClassComponent<TabStripAttrs> {
  view({attrs}: m.CVnode<TabStripAttrs>) {
    const {tabs, currentTabKey, onTabChange, className} = attrs;
    return m(
      '.pf-tabs',
      {className},
      m(
        '.pf-tabs__tabs',
        tabs.map((tab) => {
          const {key, title, leftIcon, rightIcon} = tab;
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
            return m('.pf-tabs__tab-icon', {className}, icon);
          };
          return m(
            '.pf-tabs__tab',
            {
              active: currentTabKey === key,
              key,
              onclick: () => {
                onTabChange(key);
              },
            },
            [
              renderIcon(leftIcon, 'pf-tabs__tab-icon--left'),
              m('span.pf-tabs__tab-title', title),
              renderIcon(rightIcon, 'pf-tabs__tab-icon--right'),
            ],
          );
        }),
      ),
    );
  }
}
