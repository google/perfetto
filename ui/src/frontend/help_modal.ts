
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

import {showModal} from './modal';

export function showHelp() {
  showModal({
    title: 'Perfetto Help',
    content:
        m('.help',
          m('h2', 'Navigation'),
          m('table',
            m('tr', m('td', 'W/S'), m('td', 'Zoom in/out')),
            m('tr', m('td', 'A/D'), m('td', 'Pan left/right'))),
          m('h2', 'Mouse Controls'),
          m('table',
            m('tr', m('td', 'Click'), m('td', 'Select event')),
            m('tr', m('td', 'Ctrl + Scroll wheel'), m('td', 'Zoom in/out')),
            m('tr', m('td', 'Click + Drag'), m('td', 'Pan left/right')),
            m('tr',
              m('td', 'Shift + Click + Drag'),
              m('td', 'Select a time span'))),
          m('h2', 'Other'),
          m(
              'table',
              m('tr',
                m('td', 'm (with event selected)'),
                m('td', 'Select time span of event')),
              m('tr', m('td', '?'), m('td', 'Show help')),
              )),
    buttons: [],
  });
  return;
}