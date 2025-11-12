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
import {Button, ButtonVariant} from '../../../widgets/button';
import {Popup} from '../../../widgets/popup';
import {showModal} from '../../../widgets/modal';
import {Icons} from '../../../base/semantic_icons';
import {renderDocSection, renderWidgetShowcase} from '../widgets_page_utils';

class ModalShowcase implements m.ClassComponent {
  private static counter = 0;

  private static log(txt: string) {
    const mwlogs = document.getElementById('mwlogs');
    if (!mwlogs || !(mwlogs instanceof HTMLTextAreaElement)) return;
    const time = new Date().toLocaleTimeString();
    mwlogs.value += `[${time}] ${txt}\n`;
    mwlogs.scrollTop = mwlogs.scrollHeight;
  }

  private static showModalDialog(staticContent = false) {
    const id = `N=${++ModalShowcase.counter}`;
    ModalShowcase.log(`Open ${id}`);
    const logOnClose = () => ModalShowcase.log(`Close ${id}`);

    let content;
    if (staticContent) {
      content = m(
        '.pf-modal-pre',
        'Content of the modal dialog.\nEnd of content',
      );
    } else {
      // The humble counter is basically the VDOM 'Hello world'!
      function CounterComponent() {
        let counter = 0;
        return {
          view: () => {
            return m(
              '',
              `Counter value: ${counter}`,
              m(Button, {
                label: 'Increment Counter',
                onclick: () => ++counter,
              }),
            );
          },
        };
      }
      content = () => m(CounterComponent);
    }
    const closePromise = showModal({
      title: `Modal dialog ${id}`,
      buttons: [
        {text: 'OK', action: () => ModalShowcase.log(`OK ${id}`)},
        {text: 'Cancel', action: () => ModalShowcase.log(`Cancel ${id}`)},
        {
          text: 'Show another now',
          action: () => ModalShowcase.showModalDialog(),
        },
        {
          text: 'Show another in 2s',
          action: () => setTimeout(() => ModalShowcase.showModalDialog(), 2000),
        },
      ],
      content,
    });
    closePromise.then(logOnClose);
  }

  view() {
    return m(
      'div',
      {
        style: {
          'display': 'flex',
          'flex-direction': 'column',
          'width': '100%',
        },
      },
      m('textarea', {
        id: 'mwlogs',
        readonly: 'readonly',
        rows: '8',
        placeholder: 'Logs will appear here',
      }),
      m('input[type=button]', {
        value: 'Show modal (static)',
        onclick: () => ModalShowcase.showModalDialog(true),
      }),
      m('input[type=button]', {
        value: 'Show modal (dynamic)',
        onclick: () => ModalShowcase.showModalDialog(false),
      }),
    );
  }
}

export function renderModal(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Modal'),
      m(
        'p',
        'A dialog overlay component for displaying content that requires user attention or interaction.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: () =>
        m(Button, {
          label: 'Show Modal',
          onclick: () => {
            showModal({
              title: 'Attention',
              icon: Icons.Help,
              content: () => [
                m('', 'This is a modal dialog'),
                m(
                  Popup,
                  {
                    trigger: m(Button, {
                      variant: ButtonVariant.Filled,
                      label: 'Open Popup',
                    }),
                  },
                  'Popup content',
                ),
              ],
              buttons: [
                {
                  text: 'Cancel',
                },
                {
                  text: 'OK',
                  primary: true,
                },
              ],
            });
          },
        }),
    }),

    renderDocSection('Advanced Modal', [
      m('p', 'A more advanced modal demo with logging and dynamic content.'),
    ]),

    renderWidgetShowcase({
      renderWidget: () => m(ModalShowcase),
    }),
  ];
}
