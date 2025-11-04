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
import {Icons} from '../../../base/semantic_icons';
import {
  Button,
  ButtonAttrs,
  ButtonGroup,
  ButtonVariant,
} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {Stack} from '../../../widgets/stack';
import {EnumOption, renderWidgetShowcase} from '../widgets_page_utils';
import {Anchor} from '../../../widgets/anchor';
import {CodeSnippet} from '../../../widgets/code_snippet';

function RadioButtonGroupDemo() {
  let setting: 'yes' | 'maybe' | 'no' = 'no';
  return {
    view: ({attrs}: m.Vnode<Omit<ButtonAttrs, 'label'>>) => {
      return m(ButtonGroup, [
        m(Button, {
          ...attrs,
          label: 'Yes',
          active: setting === 'yes',
          onclick: () => {
            setting = 'yes';
          },
        }),
        m(Button, {
          ...attrs,
          label: 'Maybe',
          active: setting === 'maybe',
          onclick: () => {
            setting = 'maybe';
          },
        }),
        m(Button, {
          ...attrs,
          label: 'No',
          active: setting === 'no',
          onclick: () => {
            setting = 'no';
          },
        }),
      ]);
    },
  };
}

export function renderButtonGroupDemo(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'ButtonGroup'),
      m('p', [
        'A container component that visually groups related buttons together. ',
        'Use ButtonGroup to combine actions like "Save" with a dropdown menu, ',
        'or to create radio-button-style selections where buttons have active states. ',
        'The grouped buttons are rendered with connected borders for a unified appearance.',
      ]),
      m('p', [
        'For full button documentation and usage, see ',
        m(Anchor, {href: '#!/widgets/button'}, 'Button'),
        '.',
      ]),
    ),
    renderWidgetShowcase({
      renderWidget: (opts) =>
        m(Stack, [
          m(ButtonGroup, [
            m(Button, {
              label: 'Commit',
              ...opts,
            }),
            m(Button, {
              icon: Icons.ContextMenu,
              ...opts,
            }),
          ]),
          m(RadioButtonGroupDemo, {
            ...opts,
          }),
        ]),
      initialOpts: {
        variant: new EnumOption(
          ButtonVariant.Filled,
          Object.values(ButtonVariant),
        ),
        disabled: false,
        intent: new EnumOption(Intent.None, Object.values(Intent)),
      },
    }),
    m('.pf-widget-doc-section', [
      m('h2', 'Basic Usage'),
      m(CodeSnippet, {
        text: `m(ButtonGroup, [
  m(Button, {label: 'Save'}),
  m(Button, {icon: Icons.ContextMenu}),
])`,
        language: 'typescript',
      }),
    ]),
  ];
}
