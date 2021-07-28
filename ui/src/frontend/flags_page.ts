// Copyright (C) 2020 The Android Open Source Project
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

import * as m from 'mithril';

import {featureFlags, Flag} from '../common/feature_flags';

import {globals} from './globals';
import {createPage} from './pages';

interface FlagWidgetAttrs {
  flag: Flag;
}

class FlagWidget implements m.ClassComponent<FlagWidgetAttrs> {
  view(vnode: m.Vnode<FlagWidgetAttrs>) {
    const flag = vnode.attrs.flag;
    const defaultState = flag.defaultValue ? 'Enabled' : 'Disabled';
    return m(
        '.flag-widget',
        m('label', flag.name),
        m(
            'select',
            {
              onchange: (e: InputEvent) => {
                const value = (e.target as HTMLSelectElement).value;
                switch (value) {
                  case 'enabled':
                    flag.set(true);
                    break;
                  case 'disabled':
                    flag.set(false);
                    break;
                  default:
                  case 'default':
                    flag.reset();
                    break;
                }
                globals.rafScheduler.scheduleFullRedraw();
              },
            },
            m('option',
              {value: 'default', selected: !flag.isOverridden()},
              `Default (${defaultState})`),
            m('option',
              {value: 'enabled', selected: flag.isOverridden() && flag.get()},
              'Enabled'),
            m('option',
              {value: 'disabled', selected: flag.isOverridden() && !flag.get()},
              'Disabled'),
            ),
        m('.description', flag.description),
    );
  }
}

export const FlagsPage = createPage({
  view() {
    return m(
        '.flags-page',
        m('.flags-content',
          m('h1', 'Feature flags'),
          m('button',
            {
              onclick: () => {
                featureFlags.resetAll();
                globals.rafScheduler.scheduleFullRedraw();
              },
            },
            'Reset all'),
          featureFlags.allFlags().map(flag => m(FlagWidget, {
                                        flag,
                                      }))));
  }
});
