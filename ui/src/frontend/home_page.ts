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
import {channelChanged, getNextChannel, setChannel} from '../core/channels';
import {Anchor} from '../widgets/anchor';
import {HotkeyGlyphs} from '../widgets/hotkey_glyphs';
import {assetSrc} from '../base/assets';
import {Stack} from '../widgets/stack';
import {Switch} from '../widgets/switch';
import {AppImpl} from '../core/app_impl';

export class Hints implements m.ClassComponent {
  view() {
    const themeSetting = AppImpl.instance.settings.get<string>('theme');
    const isDarkMode = themeSetting?.get() === 'dark';

    return m(
      '.pf-home-page__hints',
      m('.pf-home-page__tagline', 'New!'),
      m(
        'ul',
        m('li', [
          m(Switch, {
            label: ['Try the new dark mode.', isDarkMode && ' \u{1F60E}'],
            checked: isDarkMode,
            onchange: (e) => {
              themeSetting?.set(
                (e.target as HTMLInputElement).checked ? 'dark' : 'light',
              );
            },
          }),
        ]),
        m(
          'li',
          'Press ',
          m(HotkeyGlyphs, {hotkey: 'Mod+P'}),
          ' to quickly find tracks with fuzzy search.',
        ),
        m(
          'li',
          'Use ',
          m(
            Stack,
            {inline: true, spacing: 'small', orientation: 'horizontal'},
            [
              m(HotkeyGlyphs, {hotkey: 'W'}),
              m(HotkeyGlyphs, {hotkey: 'A'}),
              m(HotkeyGlyphs, {hotkey: 'S'}),
              m(HotkeyGlyphs, {hotkey: 'D'}),
            ],
          ),
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

export class HomePage implements m.ClassComponent {
  view() {
    return m(
      '.pf-home-page',
      m(
        '.pf-home-page__center',
        m(
          '.pf-home-page__title',
          m(`img.logo[src=${assetSrc('assets/logo-3d.png')}]`),
          'Perfetto',
        ),
        m(Hints),
        m(
          '.pf-home-page__channel-select',
          m('', 'Feeling adventurous? Try our bleeding edge Canary version'),
          m(
            'fieldset',
            mkChan('stable'),
            mkChan('canary'),
            m('.pf-home-page__highlight'),
          ),
          m(
            `.pf-home-page__reload${channelChanged() ? '.show' : ''}`,
            'You need to reload the page for the changes to have effect',
          ),
        ),
      ),
      m(
        'a.pf-privacy',
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
