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

import {channelChanged, getNextChannel, setChannel} from '../common/channels';
import {featureFlags, Flag, OverrideState} from '../common/feature_flags';

import {globals} from './globals';
import {createPage} from './pages';

const RELEASE_PROCESS_URL =
    'https://perfetto.dev/docs/visualization/perfetto-ui-release-process';

interface FlagOption {
  id: string;
  name: string;
}

interface SelectWidgetAttrs {
  label: string;
  description: m.Children;
  options: FlagOption[];
  selected: string;
  onSelect: (id: string) => void;
}

class SelectWidget implements m.ClassComponent<SelectWidgetAttrs> {
  view(vnode: m.Vnode<SelectWidgetAttrs>) {
    const attrs = vnode.attrs;
    return m(
        '.flag-widget',
        m('label', attrs.label),
        m(
            'select',
            {
              onchange: (e: InputEvent) => {
                const value = (e.target as HTMLSelectElement).value;
                attrs.onSelect(value);
                globals.rafScheduler.scheduleFullRedraw();
              },
            },
            attrs.options.map((o) => {
              const selected = o.id === attrs.selected;
              return m('option', {value: o.id, selected}, o.name);
            }),
            ),
        m('.description', attrs.description),
    );
  }
}

interface FlagWidgetAttrs {
  flag: Flag;
}

class FlagWidget implements m.ClassComponent<FlagWidgetAttrs> {
  view(vnode: m.Vnode<FlagWidgetAttrs>) {
    const flag = vnode.attrs.flag;
    const defaultState = flag.defaultValue ? 'Enabled' : 'Disabled';
    return m(SelectWidget, {
      label: flag.name,
      description: flag.description,
      options: [
        {id: OverrideState.DEFAULT, name: `Default (${defaultState})`},
        {id: OverrideState.TRUE, name: 'Enabled'},
        {id: OverrideState.FALSE, name: 'Disabled'},
      ],
      selected: flag.overriddenState(),
      onSelect: (value: string) => {
        switch (value) {
          case OverrideState.TRUE:
            flag.set(true);
            break;
          case OverrideState.FALSE:
            flag.set(false);
            break;
          default:
          case OverrideState.DEFAULT:
            flag.reset();
            break;
        }
      },
    });
  }
}

export const FlagsPage = createPage({
  view() {
    const needsReload = channelChanged();
    return m(
        '.flags-page',
        m(
            '.flags-content',
            m('h1', 'Feature flags'),
            needsReload &&
                [
                  m('h2', 'Please reload for your changes to take effect'),
                ],
            m(SelectWidget, {
              label: 'Release channel',
              description: [
                'Which release channel of the UI to use. See ',
                m('a',
                  {
                    href: RELEASE_PROCESS_URL,
                  },
                  'Release Process'),
                ' for more information.',
              ],
              options: [
                {id: 'stable', name: 'Stable (default)'},
                {id: 'canary', name: 'Canary'},
                {id: 'autopush', name: 'Autopush'},
              ],
              selected: getNextChannel(),
              onSelect: (id) => setChannel(id),
            }),
            m('button',
              {
                onclick: () => {
                  featureFlags.resetAll();
                  globals.rafScheduler.scheduleFullRedraw();
                },
              },
              'Reset all below'),

            featureFlags.allFlags().map((flag) => m(FlagWidget, {flag})),
            ));
  },
});
