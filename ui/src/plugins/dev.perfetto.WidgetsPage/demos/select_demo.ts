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
import {Select} from '../../../widgets/select';
import {renderWidgetShowcase} from '../widgets_page_utils';

export function renderSelect(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Select'),
      m('p', 'A dropdown select input for choosing one option from a list.'),
    ),
    renderWidgetShowcase({
      renderWidget: (opts) =>
        m(Select, opts, [
          m('option', {value: 'foo', label: 'Foo'}),
          m('option', {value: 'bar', label: 'Bar'}),
          m('option', {value: 'baz', label: 'Baz'}),
        ]),
      initialOpts: {
        disabled: false,
      },
    }),
  ];
}
