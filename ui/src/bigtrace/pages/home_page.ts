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

// One of the four landing-page action buttons (Quick start +
// Example queries rows). Differs only by icon, label, and click.
function homeButton(
  label: string,
  icon: string,
  onclick: () => void,
): m.Children {
  return m(
    '.pf-home-page__button',
    {onclick},
    m(Icon, {icon, className: 'pf-left-icon'}),
    m('span.pf-button__label', label),
  );
}

// Example-query button: stash the query for the new tab to pick up,
// then navigate to the editor.
function exampleQueryButton(
  label: string,
  icon: string,
  query: string,
): m.Children {
  return homeButton(label, icon, () => {
    queryState.initialQuery = query;
    setRoute(Routes.QUERY);
  });
}

export class HomePage implements m.ClassComponent {
  view() {
    const themeSetting = settingsStorage.get('theme');
    const isDarkMode = themeSetting ? themeSetting.get() === 'dark' : false;

    return m(
      '.pf-home-page',
      m(
        '.pf-home-page__center',
        // Override shared `justify-content: space-around` (shared SCSS out of scope).
        {
          style: {
            justifyContent: 'flex-start',
            paddingTop: '15vh',
            gap: '24px',
          },
        },
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
                homeButton('Configure backend', 'settings', () =>
                  setRoute(Routes.SETTINGS),
                ),
                homeButton('Open query editor', 'edit', () =>
                  setRoute(Routes.QUERY),
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
                exampleQueryButton('LMK events', 'search', LMK_QUERY),
                exampleQueryButton(
                  'Top CPU consumers',
                  'timer',
                  CPU_TIME_QUERY,
                ),
              ),
            ),
          ),
          // Footer: theme toggle; full shortcut list lives in the help modal (?).
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
