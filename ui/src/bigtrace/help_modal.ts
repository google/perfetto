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
import {assertExists} from '../base/assert';
import {HotkeyGlyphs, Keycap} from '../widgets/hotkey_glyphs';
import {showModal} from '../widgets/modal';
import {BigTraceApp} from './bigtrace_app';

export function toggleHelp(): void {
  showModal({
    title: 'BigTrace Help',
    content: () => m(BigTraceHelpContent),
  });
}

function keycap(glyph: m.Children): m.Children {
  return m(Keycap, {spacing: 'large'}, glyph);
}

class BigTraceHelpContent implements m.ClassComponent {
  view(): m.Children {
    return m(
      '.pf-help-modal',
      m('h2', 'Running SQL queries'),
      m(
        'table',
        m(
          'tr',
          m('td', keycap('Ctrl'), ' + ', keycap('Enter')),
          m('td', 'Execute query'),
        ),
        m(
          'tr',
          m('td', keycap('Ctrl'), ' + ', keycap('Enter'), ' (with selection)'),
          m('td', 'Execute selection'),
        ),
      ),
      m('h2', 'Running commands'),
      m(
        'table',
        m(
          'tr',
          m('td', keycap('>'), ' in the (empty) search box'),
          m('td', 'Switch to command mode'),
        ),
      ),
      m('h2', 'Command Hotkeys'),
      m(
        'table',
        BigTraceApp.instance.commands.commands
          .filter(({defaultHotkey}) => defaultHotkey)
          .sort((a, b) => a.name.localeCompare(b.name))
          .map(({defaultHotkey, name}) => {
            return m(
              'tr',
              m(
                'td',
                m(HotkeyGlyphs, {
                  spacing: 'large',
                  hotkey: assertExists(defaultHotkey),
                }),
              ),
              m('td', name),
            );
          }),
      ),
    );
  }
}
