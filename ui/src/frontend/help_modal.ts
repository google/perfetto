
// Copyright (C) 2019 The Android Open Source Project
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

import {globals} from './globals';
import {showModal} from './modal';

export function toggleHelp() {
  globals.logging.logEvent('User Actions', 'Show help');
  showHelp();
}

function keycap(key: string) {
  return m('.keycap', key);
}

function showHelp() {
  const ctrlOrCmd =
      window.navigator.platform.indexOf('Mac') !== -1 ? 'Cmd' : 'Ctrl';
  showModal({
    title: 'Perfetto Help',
    content: m(
        '.help',
        m('h2', 'Navigation'),
        m(
            'table',
            m(
                'tr',
                m('td', keycap('w'), '/', keycap('s')),
                m('td', 'Zoom in/out'),
                ),
            m(
                'tr',
                m('td', keycap('a'), '/', keycap('d')),
                m('td', 'Pan left/right'),
                ),
            ),
        m('h2', 'Navigation (Dvorak)'),
        m(
            'table',
            m(
                'tr',
                m('td', keycap(','), '/', keycap('o')),
                m('td', 'Zoom in/out'),
                ),
            m(
                'tr',
                m('td', keycap('a'), '/', keycap('e')),
                m('td', 'Pan left/right'),
                ),
            ),
        m('h2', 'Mouse Controls'),
        m('table',
          m('tr', m('td', 'Click'), m('td', 'Select event')),
          m('tr', m('td', 'Ctrl + Scroll wheel'), m('td', 'Zoom in/out')),
          m('tr', m('td', 'Click + Drag'), m('td', 'Select area')),
          m('tr', m('td', 'Shift + Click + Drag'), m('td', 'Pan left/right'))),
        m('h2', 'Making SQL queries from the viewer page'),
        m('table',
          m('tr',
            m('td', keycap(':'), ' in the (empty) search box'),
            m('td', 'Switch to query input')),
          m('tr', m('td', keycap('Enter')), m('td', 'Execute query')),
          m('tr',
            m('td', keycap('Ctrl'), ' + ', keycap('Enter')),
            m('td',
              'Execute query and pin output ' +
                  '(output will not be replaced by regular query input)'))),
        m('h2', 'Making SQL queries from the query page'),
        m('table',
          m('tr',
            m('td', keycap('Ctrl'), ' + ', keycap('Enter')),
            m('td', 'Execute query')),
          m('tr',
            m('td',
              keycap('Ctrl'),
              ' + ',
              keycap('Enter'),
              ' (with selection)'),
            m('td', 'Execute selection'))),
        m('h2', 'Other'),
        m(
            'table',
            m('tr',
              m('td', keycap('f'), ' (with event selected)'),
              m('td', 'Scroll + zoom to current selection')),
            m('tr',
              m('td', keycap('['), '/', keycap(']'), ' (with event selected)'),
              m('td',
                'Select next/previous slice that is connected by a flow.',
                m('br'),
                'If there are multiple flows,' +
                    'the one that is in focus (bold) is selected')),
            m('tr',
              m('td',
                keycap(ctrlOrCmd),
                ' + ',
                keycap('['),
                '/',
                keycap(']'),
                ' (with event selected)'),
              m('td', 'Switch focus to another flow')),
            m('tr',
              m('td', keycap('m'), ' (with event or area selected)'),
              m('td', 'Mark the area (temporarily)')),
            m('tr',
              m('td',
                keycap('Shift'),
                ' + ',
                keycap('m'),
                ' (with event or area selected)'),
              m('td', 'Mark the area (persistently)')),
            m('tr',
              m('td', keycap(ctrlOrCmd), ' + ', keycap('a')),
              m('td', 'Select all')),
            m('tr',
              m('td', keycap(ctrlOrCmd), ' + ', keycap('b')),
              m('td', 'Toggle display of sidebar')),
            m('tr', m('td', keycap('?')), m('td', 'Show help')),
            )),
    buttons: [],
  });
}
