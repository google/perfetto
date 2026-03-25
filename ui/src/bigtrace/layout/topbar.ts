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
import {Tooltip} from '../../widgets/tooltip';
import {settingsStorage} from '../settings/settings_storage';

class Omnibox implements m.ClassComponent {
  private value = '';

  view() {
    return m(
      '.pf-omnibox',
      m('input', {
        placeholder: 'Search or type a command...',
        style: {
          width: '500px',
        },
        value: this.value,
        oninput: (e: Event) => {
          this.value = (e.target as HTMLInputElement).value;
        },
        onkeydown: (e: KeyboardEvent) => {
          if (e.key === 'Enter') {
            this.executeCommand();
          }
        },
      }),
    );
  }

  private executeCommand() {
    const command = this.value.trim().toLowerCase();
    if (
      command === 'theme' ||
      command === 'toggle theme' ||
      command === '/theme'
    ) {
      const theme = settingsStorage.get('theme');
      if (theme) {
        const themeValue =
          (theme.get() as string) === 'light' ? 'dark' : 'light';
        theme.set(themeValue);
      }
    }
    this.value = '';
  }
}

export interface TopbarAttrs {
  sidebarVisible: boolean;
  onToggleSidebar: () => void;
  title: string;
}

export class Topbar implements m.ClassComponent<TopbarAttrs> {
  view({attrs}: m.CVnode<TopbarAttrs>) {
    return m(
      '.pf-topbar',
      {
        className: classNames(
          !attrs.sidebarVisible && 'pf-topbar--hide-sidebar',
        ),
      },
      [
        m(
          'div',
          {style: {flex: 1, display: 'flex', justifyContent: 'center'}},
          m(Omnibox),
        ),
        m(
          Tooltip,
          {
            trigger: m(
              '.pf-wip-pill',
              {
                style: {
                  position: 'absolute',
                  right: '16px',
                  fontSize: '12px',
                  color: 'var(--pf-warning-text, #856404)',
                  backgroundColor: 'var(--pf-warning-background, #fff3cd)',
                  padding: '4px 12px',
                  borderRadius: '16px',
                  fontWeight: '500',
                  border: '1px solid var(--pf-warning-border, #ffeeba)',
                  whiteSpace: 'nowrap',
                  cursor: 'pointer',
                },
              },
              'WIP',
            ),
          },
          'BigTrace UI is work in progress and may be unstable.',
        ),
      ],
    );
  }
}
