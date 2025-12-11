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
import {
  MultiParagraphText,
  TextParagraph,
} from '../../../widgets/text_paragraph';
import {renderDocSection, renderWidgetShowcase} from '../widgets_page_utils';

export function renderTextParagraph(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'TextParagraph'),
      m(
        'p',
        `A basic formatted text paragraph with wrapping. If
         it is desirable to preserve the original text format/line breaks,
         set the compressSpace attribute to false.`,
      ),
    ),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(TextParagraph, {
          text: `Lorem ipsum dolor sit amet, consectetur adipiscing
                       elit. Nulla rhoncus tempor neque, sed malesuada eros
                       dapibus vel. Aliquam in ligula vitae tortor porttitor
                       laoreet iaculis finibus est.`,
          compressSpace: opts.compressSpace,
        });
      },
      initialOpts: {
        compressSpace: true,
      },
    }),

    renderDocSection('MultiParagraphText', [
      m('p', ['A wrapper for multiple paragraph widgets.']),
    ]),

    renderWidgetShowcase({
      renderWidget: () => {
        return m(
          MultiParagraphText,
          m(TextParagraph, {
            text: `Lorem ipsum dolor sit amet, consectetur adipiscing
                       elit. Nulla rhoncus tempor neque, sed malesuada eros
                       dapibus vel. Aliquam in ligula vitae tortor porttitor
                       laoreet iaculis finibus est.`,
            compressSpace: true,
          }),
          m(TextParagraph, {
            text: `Sed ut perspiciatis unde omnis iste natus error sit
                       voluptatem accusantium doloremque laudantium, totam rem
                       aperiam, eaque ipsa quae ab illo inventore veritatis et
                       quasi architecto beatae vitae dicta sunt explicabo.
                       Nemo enim ipsam voluptatem quia voluptas sit aspernatur
                       aut odit aut fugit, sed quia consequuntur magni dolores
                       eos qui ratione voluptatem sequi nesciunt.`,
            compressSpace: true,
          }),
        );
      },
    }),
  ];
}
