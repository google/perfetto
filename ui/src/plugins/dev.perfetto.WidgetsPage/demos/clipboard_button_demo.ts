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
import {CopyToClipboardButton} from '../../../widgets/copy_to_clipboard_button';
import {EnumOption, renderWidgetShowcase} from '../widgets_page_utils';
import {ButtonVariant} from '../../../widgets/button';

export function renderClipBtnDemo(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'CopyToClipboardButton'),
      m(
        'p',
        'A button that copies predefined text to the clipboard when clicked.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: ({label, ...opts}) => {
        return m(CopyToClipboardButton, {
          textToCopy: 'Hello, World!',
          label: label ? 'Copy' : undefined,
          ...opts,
        });
      },
      initialOpts: {
        variant: new EnumOption(
          ButtonVariant.Minimal,
          Object.values(ButtonVariant),
        ),
        label: true,
      },
    }),
  ];
}
