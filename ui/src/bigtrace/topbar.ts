// Copyright (C) 2023 The Android Open Source Project
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
import {Button} from '../widgets/button';
import {settingsManager} from './settings_manager';

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
      '.bigtrace-topbar',
      {
        style: {
          gridArea: 'topbar',
          display: 'flex',
          alignItems: 'center',
          height: '30px',
          background: 'var(--pf-color-background-secondary)',
          boxShadow: 'var(--pf-box-shadow-1)',
        }
      },
      [
        !attrs.sidebarVisible && m(Button, {
          icon: 'menu',
          onclick: attrs.onToggleSidebar,
        }),
        m('span', {style: {flex: 1, textAlign: 'center'}}, attrs.title),
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
