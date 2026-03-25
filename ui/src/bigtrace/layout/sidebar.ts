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
import {Icon} from '../../widgets/icon';
import {getOrCreate} from '../../base/utils';
import {classNames} from '../../base/classnames';

export const SIDEBAR_SECTIONS = {
  home: {
    title: 'Home',
    summary: '',
    defaultCollapsed: false,
  },
  query: {
    title: 'BigTrace',
    summary: 'Query and analyze large traces',
    defaultCollapsed: false,
  },
  settings: {
    title: 'Settings',
    summary: 'Customize your BigTrace experience',
    defaultCollapsed: false,
  },
} as const;

export type SidebarSections = keyof typeof SIDEBAR_SECTIONS;

export type SidebarMenuItem = {
  readonly section: SidebarSections;
  readonly text: string;
  readonly href: string;
  readonly icon: string;
  readonly active: boolean;
  readonly onclick: () => void;
};

export interface SidebarAttrs {
  items: SidebarMenuItem[];
  onToggleSidebar: () => void;
  visible: boolean;
}

export class Sidebar implements m.ClassComponent<SidebarAttrs> {
  private _sectionExpanded = new Map<string, boolean>();

  view({attrs}: m.CVnode<SidebarAttrs>) {
    return m(
      'nav.pf-sidebar',
      {
        className: classNames(!attrs.visible && 'pf-sidebar--hidden'),
      },
      [
        m('header.pf-sidebar__header', [
          m(
            'h1',
            {style: {margin: 0, fontSize: '18px', fontWeight: 'bold'}},
            'BigTrace',
          ),
          m(
            'button.pf-sidebar-button',
            {
              onclick: attrs.onToggleSidebar,
              title: attrs.visible ? 'Hide sidebar' : 'Show sidebar',
            },
            m(Icon, {icon: 'menu'}),
          ),
        ]),
        m(
          '.pf-sidebar__content',
          Object.keys(SIDEBAR_SECTIONS).map((sectionId) =>
            this.renderSection(sectionId as SidebarSections, attrs.items),
          ),
        ),
      ],
    );
  }

  private renderSection(sectionId: SidebarSections, items: SidebarMenuItem[]) {
    const section = SIDEBAR_SECTIONS[sectionId];
    const menuItems = items
      .filter((item) => item.section === sectionId)
      .map((item) => this.renderItem(item));

    if (menuItems.length === 0) return undefined;

    const expanded = getOrCreate(
      this._sectionExpanded,
      sectionId,
      () => !section.defaultCollapsed,
    );

    return m(
      `section.pf-sidebar__section${expanded ? '.pf-sidebar__section--expanded' : ''}`,
      m(
        '.pf-sidebar__section-header',
        {
          onclick: () => {
            this._sectionExpanded.set(sectionId, !expanded);
          },
        },
        m('h1', {title: section.title}, section.title),
      ),
      m('.pf-sidebar__section-content', m('ul', menuItems)),
    );
  }

  private renderItem(item: SidebarMenuItem) {
    return m(
      'li.pf-sidebar__item',
      {
        className: classNames(item.active && 'pf-active'),
      },
      m(
        'a',
        {
          onclick: item.onclick,
          href: item.href,
        },
        [
          m(Icon, {icon: item.icon, className: 'pf-sidebar__button-icon'}),
          item.text,
        ],
      ),
    );
  }
}
