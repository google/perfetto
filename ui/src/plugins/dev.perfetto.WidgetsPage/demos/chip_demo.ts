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
import {Chip} from '../../../widgets/chip';
import {Stack} from '../../../widgets/stack';
import {Intent} from '../../../widgets/common';
import {EnumOption, renderWidgetShowcase} from '../widgets_page_utils';

export function renderChip(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Chip'),
      m(
        'p',
        'A compact tag or label component for displaying categorical information, filters, or removable selections.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        const {icon, ...rest} = opts;
        return m(
          Stack,
          {orientation: 'horizontal'},
          m(Chip, {
            label: 'Foo',
            icon: icon ? 'info' : undefined,
            ...rest,
          }),
          m(Chip, {
            label: 'Bar',
            icon: icon ? 'warning' : undefined,
            ...rest,
          }),
          m(Chip, {
            label: 'Baz',
            icon: icon ? 'error' : undefined,
            ...rest,
          }),
        );
      },
      initialOpts: {
        intent: new EnumOption(Intent.None, Object.values(Intent)),
        icon: true,
        compact: false,
        rounded: false,
        disabled: false,
        removable: true,
      },
    }),
  ];
}
