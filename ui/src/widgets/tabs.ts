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
import {Icons} from '../base/semantic_icons';
import {Button} from './button';
import {Icon} from './icon';
import {isEmptyVnodes} from '../base/mithril_utils';

export interface Tab {
  readonly key: string;
  readonly title: string;
  readonly content?: m.Children;
  readonly leftIcon?: string | m.Children;
  readonly rightIcon?: string | m.Children;
  readonly hasCloseButton?: boolean;
  readonly onClose?: () => void;
}

export interface TabsAttrs {
  readonly className?: string;
  readonly tabs: ReadonlyArray<Tab>;
  readonly currentTabKey: string;
  onTabChange(key: string): void;
  // Content to render to the left of the tabs
  readonly leftContent?: m.Children;
  // Content to render to the right of the tabs
  readonly rightContent?: m.Children;
  // If true, the tabs container will fill the available height (100%)
  readonly fillHeight?: boolean;
}

export class Tabs implements m.ClassComponent<TabsAttrs> {
  view({attrs}: m.CVnode<TabsAttrs>) {
    const {
      tabs,
      currentTabKey,
      onTabChange,
      className,
      leftContent,
      rightContent,
      fillHeight,
    } = attrs;
    const currentTab = tabs.find((t) => t.key === currentTabKey);

    return m(
      '.pf-tabs',
      {
        className,
        style: fillHeight ? {height: '100%'} : undefined,
      },
      m(
        '.pf-tabs__header',
        !isEmptyVnodes(leftContent) && m('.pf-tabs__left-content', leftContent),
        m(
          '.pf-tabs__tabs',
          tabs.map((tab) => {
            const {key, title, leftIcon, rightIcon, hasCloseButton, onClose} =
              tab;
            const isActive = currentTabKey === key;
            const renderIcon = (
              icon: string | m.Children | undefined,
              cls: string,
            ) => {
              if (icon === undefined) return undefined;
              if (typeof icon === 'string') {
                return m(Icon, {icon, className: cls});
              }
              return m('.pf-tabs__tab-icon', {className: cls}, icon);
            };
            return m(
              '.pf-tabs__tab',
              {
                active: isActive,
                key,
                onclick: () => onTabChange(key),
                onauxclick: () => onClose?.(),
              },
              [
                renderIcon(leftIcon, 'pf-tabs__tab-icon--left'),
                m('span.pf-tabs__tab-title', title),
                renderIcon(rightIcon, 'pf-tabs__tab-icon--right'),
                hasCloseButton &&
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
          }),
        ),
        !isEmptyVnodes(rightContent) &&
          m('.pf-tabs__right-content', rightContent),
      ),
      m('.pf-tabs__content', currentTab?.content),
    );
  }
}
