// Copyright (C) 2025 The Android Open Source Project
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
import {Button} from '../../../widgets/button';
import {Portal} from '../../../widgets/portal';
import {renderWidgetShowcase} from '../widgets_page_utils';

interface PortalButtonAttrs {
  readonly zIndex: boolean;
  readonly absolute: boolean;
  readonly top: boolean;
}

function PortalButton() {
  let portalOpen = false;

  return {
    view: function ({attrs}: m.Vnode<PortalButtonAttrs>): m.Children {
      const {zIndex, absolute, top} = attrs;
      return [
        m(Button, {
          label: 'Toggle Portal',
          onclick: () => {
            portalOpen = !portalOpen;
          },
        }),
        portalOpen &&
          m(
            Portal,
            {
              style: {
                position: absolute ? 'absolute' : undefined,
                top: top ? '0' : undefined,
                zIndex: zIndex ? '10' : '0',
                background: 'white',
              },
            },
            m(
              '',
              `A very simple portal - a div rendered outside of the normal
              flow of the page`,
            ),
          ),
      ];
    },
  };
}

export function renderPortal(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Portal'),
      m(
        'p',
        'A component that renders its children into a different part of the DOM tree, useful for modals and overlays.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: (opts) => m(PortalButton, opts),
      initialOpts: {
        absolute: true,
        zIndex: true,
        top: true,
      },
    }),
  ];
}
