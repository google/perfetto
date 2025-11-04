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
import {MiddleEllipsis} from '../../../widgets/middle_ellipsis';
import {renderWidgetShowcase} from '../widgets_page_utils';

export function renderMiddleEllipsis(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'MiddleEllipsis'),
      m(
        'p',
        'Truncate long text with ellipsis in the middle, preserving both the start and end of the string.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: (opts) =>
        m(
          'div',
          {style: {width: opts.squeeze ? '150px' : '450px'}},
          m(MiddleEllipsis, {
            text: 'Lorem ipsum dolor sit amet, consectetur adipiscing elit',
          }),
        ),
      initialOpts: {
        squeeze: false,
      },
    }),
  ];
}
