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
import {Card, CardStack} from '../../../widgets/card';
import {Switch} from '../../../widgets/switch';
import {EnumOption, renderWidgetShowcase} from '../widgets_page_utils';

export function cardStack(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'CardStack'),
      m(
        'p',
        `A container component that can be used to display
         multiple Card elements in a vertical stack. Cards placed in this list
         automatically have their borders adjusted to appear as one continuous
         card with thin borders between them.`,
      ),
    ),
    renderWidgetShowcase({
      renderWidget: ({direction, interactive}) =>
        m(CardStack, {direction}, [
          m(Card, {interactive}, m(Switch, {label: 'Option 1'})),
          m(Card, {interactive}, m(Switch, {label: 'Option 2'})),
          m(Card, {interactive}, m(Switch, {label: 'Option 3'})),
        ]),
      initialOpts: {
        direction: new EnumOption('vertical', [
          'vertical',
          'horizontal',
        ] as const),
        interactive: true,
      },
    }),
  ];
}
