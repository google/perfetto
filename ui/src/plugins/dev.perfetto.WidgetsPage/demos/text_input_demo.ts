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
import {TextInput} from '../../../widgets/text_input';
import {renderWidgetShowcase} from '../widgets_page_utils';

export function renderTextInput(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'TextInput'),
      m(
        'p',
        'A text input field for entering single-line text with optional placeholder and validation.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: ({placeholder, leftIcon, ...rest}) =>
        m(TextInput, {
          ...rest,
          placeholder: placeholder ? 'Placeholder...' : '',
          leftIcon: leftIcon ? 'search' : undefined,
        }),
      initialOpts: {
        placeholder: true,
        disabled: false,
        leftIcon: true,
      },
    }),
  ];
}
