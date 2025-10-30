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
import {Anchor} from '../../../widgets/anchor';
import {renderWidgetShowcase} from '../widgets_page_utils';
import {Icons} from '../../../base/semantic_icons';

export function anchor(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Anchor'),
      m('p', [
        'A hyperlink with a builtin icon to navigate to external or internal resources.',
      ]),
    ),

    renderWidgetShowcase({
      renderWidget: ({icon, showInlineWithText, long}) =>
        m('', [
          showInlineWithText && 'Inline ',
          m(
            Anchor,
            {
              icon: icon ? Icons.ExternalLink : undefined,
              href: 'https://perfetto.dev/docs/',
              target: '_blank',
            },
            long
              ? 'This is some really really really long text and hopefully it will overflow the container in order to demonstrate how long text within anchors behaves.'
              : 'Link',
          ),
          showInlineWithText && ' text',
        ]),

      initialOpts: {
        icon: true,
        showInlineWithText: false,
        long: false,
      },
    }),
  ];
}
