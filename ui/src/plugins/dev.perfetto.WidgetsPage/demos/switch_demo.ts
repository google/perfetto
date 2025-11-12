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
import {Switch} from '../../../widgets/switch';
import {renderWidgetShowcase} from '../widgets_page_utils';

export function renderSwitch(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Switch'),
      m(
        'p',
        'A toggle switch for binary on/off settings, similar to a checkbox but with a different visual style.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: ({label, labelLeft, ...rest}) =>
        m(Switch, {
          label: label ? 'Switch' : undefined,
          labelLeft: labelLeft ? 'Left Label' : undefined,
          ...rest,
        }),
      initialOpts: {
        label: true,
        labelLeft: false,
        disabled: false,
      },
    }),
  ];
}
