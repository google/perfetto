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
import {Accordion, AccordionSection} from '../../../widgets/accordion';
import {renderWidgetShowcase} from '../widgets_page_utils';

export function renderAccordion(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Accordion'),
      m(
        'p',
        'A collapsible panel component. Each section uses a native ' +
          '<details> element and manages its own open/closed state.',
      ),
    ),

    renderWidgetShowcase({
      renderWidget: ({defaultOpen, ...rest}) =>
        m(
          'div',
          {
            style: {
              border: '1px solid var(--pf-color-border)',
              borderRadius: '4px',
            },
          },
          m(
            Accordion,
            {key: defaultOpen ? 'open' : 'closed', ...rest},
            m(
              AccordionSection,
              {summary: 'Section 1', defaultOpen},
              m(
                'div',
                m('p', 'This is the content for section 1.'),
                m(
                  'p',
                  'Each section independently manages its open/closed state.',
                ),
              ),
            ),
            m(
              AccordionSection,
              {summary: 'Section 2', defaultOpen},
              m(
                'div',
                m('p', 'Content for section 2.'),
                m('p', 'Multiple sections can be open simultaneously.'),
              ),
            ),
            m(
              AccordionSection,
              {summary: 'Section 3', defaultOpen},
              m('p', 'Content for section 3. This section starts open.'),
            ),
          ),
        ),
      initialOpts: {multi: false, defaultOpen: false},
    }),
  ];
}
