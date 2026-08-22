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

import '../../frontend/topbar.scss';
import m from 'mithril';
import {assetSrc} from '../../base/assets';
import {Button} from '../../widgets/button';
import {toggleHelp} from '../help_modal';
import {ConnectionButton} from './connection_button';
import {Omnibox} from './omnibox';

// The app's only chrome: brand, command omnibox, and the backend connection.
export class Topbar implements m.ClassComponent {
  view() {
    return m('.pf-topbar', [
      m('.pf-bt-topbar-brand', [
        m('img.pf-bt-topbar-logo', {src: assetSrc('assets/logo-128.png')}),
        'BigTrace',
      ]),
      m(Omnibox),
      m('.pf-topbar__right', [
        m(ConnectionButton),
        m(Button, {
          icon: 'help_outline',
          title: 'Help (?)',
          onclick: () => toggleHelp(),
        }),
      ]),
    ]);
  }
}
