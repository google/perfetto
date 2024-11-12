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

import m from 'mithril';
import {raf} from '../core/raf_scheduler';
import {AppImpl} from '../core/app_impl';

const COOKIE_ACK_KEY = 'cookieAck';

export class CookieConsent implements m.ClassComponent {
  private showCookieConsent = true;

  oninit() {
    this.showCookieConsent = true;
    if (
      !AppImpl.instance.analytics.isEnabled() ||
      localStorage.getItem(COOKIE_ACK_KEY) === 'true'
    ) {
      this.showCookieConsent = false;
    }
  }

  view() {
    if (!this.showCookieConsent) return;
    return m(
      '.cookie-consent',
      m(
        '.cookie-text',
        `This site uses cookies from Google to deliver its services and to
          analyze traffic.`,
      ),
      m(
        '.buttons',
        m(
          'button',
          m(
            'a',
            {
              href: 'https://policies.google.com/technologies/cookies',
              target: '_blank',
            },
            'More details',
          ),
        ),
        m(
          'button',
          {
            onclick: () => {
              this.showCookieConsent = false;
              localStorage.setItem(COOKIE_ACK_KEY, 'true');
              raf.scheduleFullRedraw();
            },
          },
          'OK',
        ),
      ),
    );
  }
}
