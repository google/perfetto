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
import {assetSrc} from '../../base/assets';
import {Icon} from '../../widgets/icon';
import {HotkeyGlyphs} from '../../widgets/hotkey_glyphs';
import {Switch} from '../../widgets/switch';
import {queryState} from '../query/query_state';
import {settingsStorage} from '../settings/settings_storage';
import {setRoute} from '../router';
import {Routes} from '../routes';

const LMK_QUERY = `INCLUDE PERFETTO MODULE android.memory.lmk;

SELECT 
  *
FROM android_lmk_events
WHERE oom_score_adj <= 200
ORDER BY oom_score_adj
LIMIT 1;`;

const CPU_TIME_QUERY = `SELECT
  p.name,
  sum(s.dur)/1e9 as cpu_sec
FROM sched s
JOIN thread t USING (utid)
JOIN process p USING (upid)
GROUP BY p.name
ORDER BY cpu_sec DESC
LIMIT 10`;

export class HomePage implements m.ClassComponent {
  view() {
    const themeSetting = settingsStorage.get('theme');
    const isDarkMode = themeSetting ? themeSetting.get() === 'dark' : false;

    return m(
      '.pf-home-page',
      m(
        '.pf-home-page__center',
        m(
          '.pf-home-page__title',
          m(`img.logo[src=${assetSrc('assets/logo-3d.png')}]`),
          'BigTrace',
        ),
        m(
          '.pf-home-page__hints',
          // Quick start section
          m(
            '.pf-home-page__section',
            m('.pf-home-page__section-title', 'Quick start'),
            m(
              '.pf-home-page__section-content',
              m(
                '.pf-home-page__getting-started-buttons',
                m(
                  '.pf-home-page__button',
                  {onclick: () => setRoute(Routes.SETTINGS)},
                  m(Icon, {icon: 'settings', className: 'pf-left-icon'}),
                  m('span.pf-button__label', 'Configure targets'),
                ),
                m(
                  '.pf-home-page__button',
                  {onclick: () => setRoute(Routes.QUERY)},
                  m(Icon, {icon: 'edit', className: 'pf-left-icon'}),
                  m('span.pf-button__label', 'Open query editor'),
                ),
              ),
            ),
          ),
          // Example queries section
          m(
            '.pf-home-page__section',
            m('.pf-home-page__section-title', 'Example queries'),
            m(
              '.pf-home-page__section-content',
              m(
                '.pf-home-page__getting-started-buttons',
                m(
                  '.pf-home-page__button',
                  {
                    onclick: () => {
                      queryState.initialQuery = LMK_QUERY;
                      setRoute(Routes.QUERY);
                    },
                  },
                  m(Icon, {icon: 'search', className: 'pf-left-icon'}),
                  m('span.pf-button__label', 'LMK events'),
                ),
                m(
                  '.pf-home-page__button',
                  {
                    onclick: () => {
                      queryState.initialQuery = CPU_TIME_QUERY;
                      setRoute(Routes.QUERY);
                    },
                  },
                  m(Icon, {icon: 'timer', className: 'pf-left-icon'}),
                  m('span.pf-button__label', 'Top CPU consumers'),
                ),
              ),
            ),
          ),
          // Shortcuts section
          m(
            '.pf-home-page__section',
            m('.pf-home-page__section-title', 'Shortcuts'),
            m(
              '.pf-home-page__section-content',
              m(
                '.pf-home-page__shortcut',
                m('span.pf-home-page__shortcut-label', 'Commands'),
                m(HotkeyGlyphs, {hotkey: '!Mod+Shift+P'}),
              ),
              m(
                '.pf-home-page__shortcut',
                m('span.pf-home-page__shortcut-label', 'Toggle sidebar'),
                m(HotkeyGlyphs, {hotkey: '!Mod+B'}),
              ),
            ),
          ),
          // Links below the cards
          m(
            '.pf-home-page__links',
            m(Switch, {
              label: 'Dark mode',
              checked: isDarkMode,
              onchange: (e) => {
                themeSetting?.set(
                  (e.target as HTMLInputElement).checked ? 'dark' : 'light',
                );
              },
            }),
          ),
        ),
      ),
    );
  }
}
