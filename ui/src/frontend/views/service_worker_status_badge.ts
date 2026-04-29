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
import {AppImpl} from '../../core/app_impl';
import {showModal} from '../../widgets/modal';

export interface ServiceWorkerStatusBadgeAttrs {
  readonly app: AppImpl;
}

export const ServiceWorkerStatusBadge: m.ClassComponent<ServiceWorkerStatusBadgeAttrs> =
  {
    view({attrs}: m.CVnode<ServiceWorkerStatusBadgeAttrs>) {
      let modifier: string | undefined;
      let title = 'Service Worker: ';
      let label = 'N/A';
      const ctl = attrs.app.serviceWorkerController;
      if (!('serviceWorker' in navigator)) {
        title += 'not supported by the browser (requires HTTPS)';
      } else if (ctl.bypassed) {
        label = 'OFF';
        modifier = 'pf-sidebar__dbg-info-square--red';
        title += 'Bypassed, using live network. Double-click to re-enable';
      } else if (ctl.installing) {
        label = 'UPD';
        modifier = 'pf-sidebar__dbg-info-square--amber';
        title += 'Installing / updating ...';
      } else if (!navigator.serviceWorker.controller) {
        title += 'Not available, using network';
      } else {
        label = 'ON';
        modifier = 'pf-sidebar__dbg-info-square--green';
        title += 'Serving from cache. Ready for offline use';
      }

      const toggle = async () => {
        if (ctl.bypassed) {
          ctl.setBypass(false);
          return;
        }
        showModal({
          title: 'Disable service worker?',
          content: m(
            'div',
            m(
              'p',
              `If you continue the service worker will be disabled until
                      manually re-enabled.`,
            ),
            m(
              'p',
              `All future requests will be served from the network and the
                    UI won't be available offline.`,
            ),
            m(
              'p',
              `You should do this only if you are debugging the UI
                    or if you are experiencing caching-related problems.`,
            ),
            m(
              'p',
              `Disabling will cause a refresh of the UI, the current state
                    will be lost.`,
            ),
          ),
          buttons: [
            {
              text: 'Disable and reload',
              primary: true,
              action: () => ctl.setBypass(true).then(() => location.reload()),
            },
            {text: 'Cancel'},
          ],
        });
      };

      return m(
        '.pf-sidebar__dbg-info-square',
        {className: modifier, title, ondblclick: toggle},
        m('div', 'SW'),
        m('div', label),
      );
    },
  };
