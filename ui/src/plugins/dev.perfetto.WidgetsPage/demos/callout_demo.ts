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
import {Callout} from '../../../widgets/callout';
import {Intent} from '../../../widgets/common';
import {EnumOption, renderWidgetShowcase} from '../widgets_page_utils';

export function renderCallout(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Callout'),
      m(
        'p',
        'An attention-grabbing banner for displaying important messages with optional icons and dismiss functionality.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: ({icon, ...opts}) =>
        m(
          Callout,
          {
            icon: icon ? 'info' : undefined,
            ...opts,
          },
          'Lorem ipsum dolor sit amet, consectetur adipiscing elit. ' +
            'Nulla rhoncus tempor neque, sed malesuada eros dapibus vel. ' +
            'Aliquam in ligula vitae tortor porttitor laoreet iaculis ' +
            'finibus est.',
        ),
      initialOpts: {
        intent: new EnumOption(Intent.None, Object.values(Intent)),
        dismissible: false,
        icon: true,
      },
    }),
  ];
}
