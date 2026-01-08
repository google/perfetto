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
import {Button, ButtonBar} from '../../../widgets/button';
import {Popup, PopupPosition} from '../../../widgets/popup';
import {PopupMenu} from '../../../widgets/menu';
import {MenuItem} from '../../../widgets/menu';
import {
  EnumOption,
  renderDocSection,
  renderWidgetShowcase,
} from '../widgets_page_utils';
import {Anchor} from '../../../widgets/anchor';

function lorem() {
  const text = `Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod
      tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim
      veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea
      commodo consequat.Duis aute irure dolor in reprehenderit in voluptate
      velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat
      cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id
      est laborum.`;
  return m('', {style: {width: '200px'}}, text);
}

export function renderPopup(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Popup'),
      m(
        'p',
        'A floating overlay component that appears relative to a trigger element, used for tooltips, dropdowns, and popovers. ',
        'Based on ',
        m(Anchor, {href: '#!/widgets/portal'}, 'Portal'),
        '.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: ({...rest}) =>
        m(
          Popup,
          {
            trigger: m(Button, {label: 'Toggle popup'}),
            ...rest,
          },
          lorem(),
        ),
      initialOpts: {
        position: new EnumOption(
          PopupPosition.Auto,
          Object.values(PopupPosition),
        ),
        closeOnEscape: true,
        closeOnOutsideClick: true,
        isContextMenu: false,
        positionAtCursor: false,
      },
    }),

    renderDocSection('Controlled Popup', [
      m('p', [
        `The open/close state of a controlled popup is passed in via
      the 'isOpen' attribute. This means we can get open or close the popup
      from wherever we like. E.g. from a button inside the popup.
      Keeping this state external also means we can modify other parts of the
      page depending on whether the popup is open or not, such as the text
      on this button.
      Note, this is the same component as the popup above, but used in
      controlled mode.`,
      ]),
    ]),

    renderWidgetShowcase({
      renderWidget: ({isOpen}) =>
        m(
          Popup,
          {
            trigger: m(Button, {
              disabled: true,
              label: `Popup is ${isOpen ? 'open' : 'closed'}`,
            }),
            isOpen,
          },
          lorem(),
        ),
      initialOpts: {
        isOpen: false,
      },
    }),

    renderDocSection('Nested Popups', [
      m('p', [`Popups can be nested inside other popups. Here is an example.`]),
    ]),

    renderWidgetShowcase({
      renderWidget: () =>
        m(
          Popup,
          {
            trigger: m(Button, {label: 'Toggle nested popup'}),
          },
          m(ButtonBar, [
            m(
              PopupMenu,
              {
                trigger: m(Button, {label: 'Select an option'}),
              },
              m(MenuItem, {label: 'Option 1'}),
              m(MenuItem, {label: 'Option 2'}),
            ),
          ]),
        ),
    }),
  ];
}
