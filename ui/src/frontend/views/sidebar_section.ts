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
import {classNames} from '../../base/classnames';
import {AppImpl} from '../../core/app_impl';
import {SidebarMenuItemInternal} from '../../core/sidebar_manager';
import {SidebarItem} from './sidebar_item';

export interface SidebarSectionAttrs {
  readonly app: AppImpl;
  readonly title: string;
  readonly summary: string;
  readonly items: readonly SidebarMenuItemInternal[];
  readonly defaultCollapsed?: boolean;
  // Optional content to render above the items (e.g. the trace file name).
  readonly leading?: m.Children;
}

export class SidebarSection implements m.ClassComponent<SidebarSectionAttrs> {
  private expanded?: boolean;

  view({attrs}: m.CVnode<SidebarSectionAttrs>): m.Children {
    if (attrs.items.length === 0) return undefined;

    if (this.expanded === undefined) {
      this.expanded = !attrs.defaultCollapsed;
    }
    const expanded = this.expanded;

    const menuItems = attrs.items
      .slice()
      .sort((a, b) => (a.sortOrder ?? 0) - (b.sortOrder ?? 0))
      .map((item) => m(SidebarItem, {app: attrs.app, item}));

    return m(
      'section.pf-sidebar__section',
      {
        className: classNames(expanded && 'pf-sidebar__section--expanded'),
      },
      m(
        '.pf-sidebar__section-header',
        {onclick: () => (this.expanded = !expanded)},
        m('h1', {title: attrs.title}, attrs.title),
        m('h2', attrs.summary),
      ),
      m('.pf-sidebar__section-content', attrs.leading, m('ul', menuItems)),
    );
  }
}
