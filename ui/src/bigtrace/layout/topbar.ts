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
import {Button} from '../../widgets/button';
import { Tooltip } from '../../widgets/tooltip';
import {settingsManager} from '../settings/settings_manager';

class Omnibox implements m.ClassComponent {
  view() {
    return m(
      '.pf-omnibox',
      m('input', {
        placeholder: 'Search',
        style: {
          width: '500px',
        },
      }),
    );
  }
}

export interface TopbarAttrs {
  sidebarVisible: boolean;
  onToggleSidebar: () => void;
  title: string;
}

export class Topbar implements m.ClassComponent<TopbarAttrs> {
  view({attrs}: m.CVnode<TopbarAttrs>) {
    const theme = settingsManager.get('theme');
    const themeValue = theme ? theme.get() : 'light';

    return m(
      '.pf-topbar',
      {
        className: classNames(
          !attrs.sidebarVisible && 'pf-topbar--hide-sidebar',
        ),
      },
      [
        !attrs.sidebarVisible && m(Button, {
          icon: 'menu',
          onclick: attrs.onToggleSidebar,
          style: {height: '48px', width: '48px'},
        }),
        m(Tooltip, {
          trigger: m('.pf-wip-pill', {
            style: {
              fontSize: '12px',
              color: 'var(--pf-warning-text, #856404)',
              backgroundColor: 'var(--pf-warning-background, #fff3cd)',
              padding: '4px 12px',
              borderRadius: '16px',
              marginLeft: '16px',
              fontWeight: '500',
              border: '1px solid var(--pf-warning-border, #ffeeba)',
              whiteSpace: 'nowrap',
              cursor: 'pointer',
            },
          }, 'WIP')
        }, 'BigTrace UI is work in progress. Features are subject to change.'),
        m('div', {style: {flex: 1, display: 'flex', justifyContent: 'center'}}, m(Omnibox)),
        
        m(Button, {
          icon: themeValue === 'light' ? 'dark_mode' : 'light_mode',
          onclick: () => {
            if (theme) {
              theme.set(themeValue === 'light' ? 'dark' : 'light');
            }
          },
        }),
      ],
    );
  }
}

