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
import './home_page.scss';
import {AppImpl} from '../../../ui/src/core/app_impl';
import {Button, ButtonVariant} from '../../../ui/src/widgets/button';
import {Icon} from '../../../ui/src/widgets/icon';
import {Intent} from '../../../ui/src/widgets/common';

export class SysmemHomePage implements m.ClassComponent {
  view() {
    const app = AppImpl.instance;
    return m(
      '.pf-sysmem-home',
      m(Icon, {icon: 'developer_board', className: 'pf-sysmem-home__logo'}),
      m('h1', 'Sysmem'),
      m(
        'p.pf-sysmem-home__tagline',
        "See where a whole device's memory goes, live: which processes use " +
          'it, how much is page cache, and when the system is under pressure.',
      ),
      m(
        'p',
        'Connect an Android phone over USB or a Linux machine running traced.',
      ),
      m(Button, {
        label: 'Connect a device',
        icon: 'usb',
        intent: Intent.Primary,
        variant: ButtonVariant.Filled,
        onclick: () => app.navigate('#!/sysmem'),
      }),
      m(
        'p.pf-sysmem-home__hint',
        'To debug the memory of one app, use Memscope.',
      ),
    );
  }
}
