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
import {Button, ButtonVariant} from '../../../widgets/button';
import {Card} from '../../../widgets/card';
import {Intent} from '../../../widgets/common';
import {Stack} from '../../../widgets/stack';
import {renderWidgetShowcase} from '../widgets_page_utils';

export function renderCard(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Card'),
      m(
        'p',
        `A card is a simple container with a shadow and rounded
        corners. It can be used to display grouped content in a visually
        appealing way.`,
      ),
    ),
    renderWidgetShowcase({
      renderWidget: ({interactive}) =>
        m(Card, {interactive}, [
          m('h1', {style: {margin: 'unset'}}, 'Welcome!'),
          m('p', 'Would you like to start your journey?'),
          m(Stack, {orientation: 'horizontal'}, [
            m(Button, {
              variant: ButtonVariant.Filled,
              label: 'No thanks...',
            }),
            m(Button, {
              intent: Intent.Primary,
              variant: ButtonVariant.Filled,
              label: "Let's go!",
            }),
          ]),
        ]),
      initialOpts: {interactive: true},
    }),
  ];
}
