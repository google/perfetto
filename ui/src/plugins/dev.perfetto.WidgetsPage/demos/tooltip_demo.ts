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
import {Icon} from '../../../widgets/icon';
import {PopupPosition} from '../../../widgets/popup';
import {Tooltip} from '../../../widgets/tooltip';
import {EnumOption, renderWidgetShowcase} from '../widgets_page_utils';

function lorem() {
  const text = `Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod
      tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim
      veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea
      commodo consequat.Duis aute irure dolor in reprehenderit in voluptate
      velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat
      cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id
      est laborum.`;
  return m('', {style: {width: '200px'}}, text);
}

export function renderTooltip(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Tooltip'),
      m(
        'p',
        'A floating tooltip component that appears on hover to provide additional information about an element.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: ({...rest}) =>
        m(
          Tooltip,
          {
            trigger: m(Icon, {icon: 'Warning'}),
            ...rest,
          },
          lorem(),
        ),
      initialOpts: {
        position: new EnumOption(
          PopupPosition.Auto,
          Object.values(PopupPosition),
        ),
        showArrow: true,
        offset: 0,
        edgeOffset: 0,
      },
    }),
  ];
}
