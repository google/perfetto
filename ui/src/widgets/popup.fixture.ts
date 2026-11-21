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

// Component harness fixtures for Popup. Only bundled into the component
// harness (ui/build --enable-component-harness), never shipped.

import m from 'mithril';
import type {FixtureModule} from '../component_harness/registry';
import {Button} from './button';
import {Popup, PopupPosition} from './popup';

const POPUP_CONTENT =
  'Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod ' +
  'tempor incididunt ut labore et dolore magna aliqua.';

function openPopup(): m.Children {
  return m(
    Popup,
    {
      trigger: m(Button, {label: 'Trigger'}),
      isOpen: true,
      position: PopupPosition.Right,
    },
    m('', {style: {maxWidth: '240px'}}, POPUP_CONTENT),
  );
}

export default {
  // An open popup whose trigger lives in a scroll box (.scroll-container).
  // The box starts scrolled to the middle with the trigger in view, so it can
  // be scrolled out of view in either direction.
  scroll_container: () => ({
    view: () =>
      m(
        '.scroll-container',
        {
          style: {
            height: '200px',
            width: '300px',
            overflow: 'auto',
            border: '1px solid var(--pf-color-border)',
          },
          oncreate: ({dom}: m.VnodeDOM) => {
            dom.scrollTop = (dom.scrollHeight - dom.clientHeight) / 2;
          },
        },
        m('', {style: {padding: '400px 16px'}}, openPopup()),
      ),
  }),

  // An open popup whose trigger lives in a container (.trigger-container)
  // that the 'Toggle container' button hides with display:none. The toggle
  // goes through mithril (as it would in the app), so the popup gets redrawn.
  hideable_container: () => {
    let hidden = false;
    return {
      view: () =>
        m('', [
          m(Button, {
            label: 'Toggle container',
            onclick: () => (hidden = !hidden),
          }),
          m(
            '.trigger-container',
            {
              style: {
                display: hidden ? 'none' : 'block',
                marginTop: '8px',
                padding: '16px',
                border: '1px solid var(--pf-color-border)',
              },
            },
            openPopup(),
          ),
        ]),
    };
  },
} satisfies FixtureModule;
