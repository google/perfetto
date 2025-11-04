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
import {CodeSnippet} from '../../../widgets/code_snippet';
import {Intent} from '../../../widgets/common';
import {EnumOption, renderWidgetShowcase} from '../widgets_page_utils';

export function renderButtonDemo(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Button'),
      m('p', [
        'A versatile button component with multiple variants, intents, and states. ',
        'Supports icons, loading states, and can be grouped with other buttons.',
      ]),
    ),

    renderWidgetShowcase({
      renderWidget: ({
        label,
        icon,
        rightIcon,
        showAsGrid,
        showInlineWithText,
        ...rest
      }) =>
        showAsGrid
          ? m(
              '',
              {
                style: {
                  display: 'grid',
                  gridTemplateColumns: 'auto auto auto',
                  gap: '4px',
                },
              },
              Object.values(Intent).map((intent) => {
                return Object.values(ButtonVariant).map((variant) => {
                  return m(Button, {
                    style: {
                      width: '80px',
                    },
                    ...rest,
                    label: variant,
                    variant,
                    intent,
                  });
                });
              }),
            )
          : m('', [
              showInlineWithText && 'Inline ',
              m(Button, {
                icon: icon ? 'send' : undefined,
                rightIcon: rightIcon ? 'arrow_forward' : undefined,
                label: (label ? 'Button' : undefined) as string,
                onclick: () => console.log('button pressed'),
                ...rest,
              }),
              showInlineWithText && ' text',
            ]),
      initialOpts: {
        label: true,
        icon: true,
        rightIcon: false,
        disabled: false,
        intent: new EnumOption(Intent.None, Object.values(Intent)),
        active: false,
        compact: false,
        loading: false,
        variant: new EnumOption(
          ButtonVariant.Filled,
          Object.values(ButtonVariant),
        ),
        showAsGrid: false,
        showInlineWithText: false,
        rounded: false,
      },
    }),

    m('.pf-widget-doc-section', [
      m('h2', 'Basic Usage'),
      m('p', 'Create buttons with labels, icons, or both:'),
      m(CodeSnippet, {
        text: `// Simple button with label
m(Button, {
  label: 'Click me',
  onclick: () => console.log('clicked'),
})

// Button with icon
m(Button, {
  icon: 'send',
  label: 'Send',
  onclick: handleSend,
})

// Icon-only button
m(Button, {
  icon: 'delete',
  onclick: handleDelete,
})`,
        language: 'typescript',
      }),
    ]),

    m('.pf-widget-doc-section', [
      m('h2', 'Variants & Intents'),
      m('p', 'Buttons come in multiple visual styles:'),
      m(CodeSnippet, {
        text: `// Variants
m(Button, {label: 'Filled', variant: ButtonVariant.Filled})
m(Button, {label: 'Outlined', variant: ButtonVariant.Outlined})
m(Button, {label: 'Subtle', variant: ButtonVariant.Subtle})

// Intents (colors)
m(Button, {label: 'Primary', intent: Intent.Primary})
m(Button, {label: 'Danger', intent: Intent.Danger})`,
        language: 'typescript',
      }),
    ]),

    m('.pf-widget-doc-section', [
      m('h2', 'Key Features'),
      m('ul', [
        m('li', [
          m('strong', 'Multiple Variants: '),
          'Filled, Outlined, and Subtle styles for different visual weights',
        ]),
        m('li', [
          m('strong', 'Intent Colors: '),
          'Primary, Danger, and default intents for semantic meaning',
        ]),
        m('li', [
          m('strong', 'Icons: '),
          'Left icon, right icon, or icon-only buttons',
        ]),
        m('li', [
          m('strong', 'States: '),
          'Active, disabled, and loading states',
        ]),
        m('li', [
          m('strong', 'Compact Mode: '),
          'Smaller padding for dense UIs',
        ]),
        m('li', [
          m('strong', 'Rounded: '),
          'Circular icon buttons for floating actions',
        ]),
        m('li', [
          m('strong', 'Button Groups: '),
          'Group related buttons with ButtonGroup or SegmentedButtons',
        ]),
      ]),
    ]),
  ];
}
