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
  SegmentedButton,
  SegmentedButtons,
} from '../../../widgets/segmented_buttons';
import {EnumOption, renderWidgetShowcase} from '../widgets_page_utils';
import {Intent} from '../../../widgets/common';

export function segmentedButtons(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'SegmentedButtons'),
      m('p', [
        `Segmented buttons are a group of buttons where one of them is
         'selected'; they act similar to a set of radio buttons.`,
      ]),
    ),

    renderWidgetShowcase({
      renderWidget: (opts) => {
        const {showInlineText, icons, ...rest} = opts;
        const icon = icons ? 'add' : undefined;
        const buttons = m(SegmentedButtons, rest, [
          m(SegmentedButton, {icon, value: 'yes'}, 'Yes'),
          m(SegmentedButton, {icon, value: 'no'}, 'No'),
          m(SegmentedButton, {icon, value: 'maybe'}, 'Maybe'),
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
