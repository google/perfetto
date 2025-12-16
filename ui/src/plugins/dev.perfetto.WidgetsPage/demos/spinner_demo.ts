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
import {Spinner} from '../../../widgets/spinner';
import {EnumOption, renderWidgetShowcase} from '../widgets_page_utils';

export function renderSpinner(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Spinner'),
      m(
        'p',
        'A loading indicator that shows indeterminate progress. Width and height match the font size.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: ({fontSize, easing}) =>
        m('', {style: {fontSize}}, m(Spinner, {easing})),
      initialOpts: {
        fontSize: new EnumOption('16px', [
          '12px',
          '16px',
          '24px',
          '32px',
          '64px',
          '128px',
        ]),
        easing: false,
      },
    }),
  ];
}
