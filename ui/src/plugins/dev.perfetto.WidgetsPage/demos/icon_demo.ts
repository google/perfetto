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
import {Intent} from '../../../widgets/common';
import {Icon} from '../../../widgets/icon';
import {EnumOption, renderWidgetShowcase} from '../widgets_page_utils';

export function renderIcon(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Icon'),
      m('p', 'Display Material Design icons with customizable size and color.'),
    ),
    renderWidgetShowcase({
      renderWidget: (opts) => m(Icon, {icon: 'star', ...opts}),
      initialOpts: {
        filled: false,
        intent: new EnumOption(Intent.None, Object.values(Intent)),
      },
    }),
  ];
}
