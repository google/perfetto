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
import {Accordion, AccordionItem} from '../../../widgets/accordion';

const DEMO_ITEMS: AccordionItem[] = [
  {
    id: 'section1',
    header: 'Section 1',
    content: m(
      'div',
      m('p', 'This is the content for section 1.'),
      m('p', 'The accordion ensures only one section is expanded at a time.'),
    ),
  },
  {
    id: 'section2',
    header: 'Section 2',
    content: m(
      'div',
      m('p', 'Content for section 2.'),
      m('p', 'Click another header to collapse this and expand that one.'),
    ),
  },
  {
    id: 'section3',
    header: 'Section 3',
    content: m('p', 'Content for section 3.'),
  },
];

export function renderAccordion(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Accordion'),
      m(
        'p',
        'A collapsible panel component that displays a list of items where ' +
          'only one item can be expanded at a time. Supports both controlled ' +
          'and uncontrolled modes.',
      ),
    ),
    m('h2', 'Uncontrolled Mode'),
    m(
      'p',
      'In uncontrolled mode, the accordion manages its own expanded state internally. ' +
        'All items start collapsed.',
    ),
    m(
      'div',
      {
        style: {
          border: '1px solid var(--pf-color-border)',
          borderRadius: '4px',
        },
      },
      m(Accordion, {items: DEMO_ITEMS}),
    ),
  ];
}
