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
import {RadioGroup} from '../../../widgets/radio_group';
import {EnumOption, renderWidgetShowcase} from '../widgets_page_utils';
import {Intent} from '../../../widgets/common';

export function radioGroup(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'RadioGroup'),
      m('p', [
        `A group of mutually-exclusive buttons, equivalent to a set of radio
         buttons rendered as a segmented control.`,
      ]),
    ),

    renderWidgetShowcase({
      renderWidget: (opts) => {
        const {showInlineText, icons, ...rest} = opts;
        const icon = icons ? 'add' : undefined;
        const buttons = m(RadioGroup, rest, [
          m(RadioGroup.Button, {icon, value: 'yes'}, 'Yes'),
          m(RadioGroup.Button, {icon, value: 'no'}, 'No'),
          m(RadioGroup.Button, {icon, value: 'maybe'}, 'Maybe'),
        ]);
        return showInlineText
          ? m('span', 'Inline ', buttons, ' text')
          : buttons;
      },
      initialOpts: {
        disabled: false,
        fillWidth: false,
        showInlineText: false,
        intent: new EnumOption(Intent.None, Object.values(Intent)),
        icons: false,
      },
    }),
  ];
}
