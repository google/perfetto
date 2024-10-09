// Copyright (C) 2018 The Android Open Source Project
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
import {channelChanged, getNextChannel, setChannel} from '../common/channels';
import {Anchor} from '../widgets/anchor';
import {HotkeyGlyphs} from '../widgets/hotkey_glyphs';
import {globals} from './globals';
import {PageAttrs} from '../core/router';

export class Hints implements m.ClassComponent {
  view() {
    return m(
      '.home-page-hints',
      m('.tagline', 'New!'),
      m(
        'ul',
        m(
          'li',
          'New updated ',
          m(
            Anchor,
            {
              href: 'https://perfetto.dev/docs/visualization/perfetto-ui#tabs-v2',
            },
            'tabs',
          ),
          ' are extensible and user friendly.',
        ),
        m(
          'li',
          'Use ',
          m(HotkeyGlyphs, {hotkey: 'W'}),
          m(HotkeyGlyphs, {hotkey: 'A'}),
          m(HotkeyGlyphs, {hotkey: 'S'}),
          m(HotkeyGlyphs, {hotkey: 'D'}),
          ' to navigate the trace.',
        ),
        m(
          'li',
          'Try the ',
          m(
            Anchor,
            {
              href: 'https://perfetto.dev/docs/visualization/perfetto-ui#command-palette',
            },
            'command palette,',
          ),
          ' press ',
          m(HotkeyGlyphs, {hotkey: '!Mod+Shift+P'}),
          '.',
        ),
      ),
    );
  }
}

export class HomePage implements m.ClassComponent<PageAttrs> {
  view() {
    return m(
      '.page.home-page',
      m(
        '.home-page-center',
        m(
          '.home-page-title',
          m(`img.logo[src=${globals.root}assets/logo-3d.png]`),
          'Perfetto',
        ),
        m(Hints),
        m(
          '.channel-select',
          m('', 'Feeling adventurous? Try our bleeding edge Canary version'),
          m('fieldset', mkChan('stable'), mkChan('canary'), m('.highlight')),
          m(
            `.home-page-reload${channelChanged() ? '.show' : ''}`,
            'You need to reload the page for the changes to have effect',
          ),
        ),
      ),
      m(
        'a.privacy',
        {href: 'https://policies.google.com/privacy', target: '_blank'},
        'Privacy policy',
      ),
    );
  }
}

function mkChan(chan: string) {
  const checked = getNextChannel() === chan ? '[checked=true]' : '';
  return [
    m(`input[type=radio][name=chan][id=chan_${chan}]${checked}`, {
      onchange: () => {
        setChannel(chan);
      },
    }),
    m(`label[for=chan_${chan}]`, chan),
  ];
}
