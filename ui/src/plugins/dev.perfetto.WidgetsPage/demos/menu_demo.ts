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
import {Menu, MenuDivider, MenuItem} from '../../../widgets/menu';
import {renderWidgetShowcase} from '../widgets_page_utils';

export function renderMenu(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Menu'),
      m(
        'p',
        'A dropdown menu component with menu items, supporting nested submenus and keyboard navigation.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: () =>
        m(
          Menu,
          m(MenuItem, {label: 'New', icon: 'add'}),
          m(MenuItem, {label: 'Open', icon: 'folder_open'}),
          m(MenuItem, {label: 'Save', icon: 'save', disabled: true}),
          m(MenuDivider),
          m(MenuItem, {label: 'Delete', icon: 'delete'}),
          m(MenuDivider),
          m(
            MenuItem,
            {label: 'Share', icon: 'share'},
            m(MenuItem, {label: 'Everyone', icon: 'public'}),
            m(MenuItem, {label: 'Friends', icon: 'group'}),
            m(
              MenuItem,
              {label: 'Specific people', icon: 'person_add'},
              m(MenuItem, {label: 'Alice', icon: 'person'}),
              m(MenuItem, {label: 'Bob', icon: 'person'}),
            ),
          ),
          m(
            MenuItem,
            {label: 'More', icon: 'more_horiz'},
            m(MenuItem, {label: 'Query', icon: 'database'}),
            m(MenuItem, {label: 'Download', icon: 'download'}),
            m(MenuItem, {label: 'Clone', icon: 'copy_all'}),
          ),
        ),
    }),
  ];
}
