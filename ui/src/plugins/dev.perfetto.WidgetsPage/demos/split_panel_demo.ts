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
import {Button} from '../../../widgets/button';
import {SplitPanel, Tab} from '../../../widgets/split_panel';
import {renderWidgetShowcase} from '../widgets_page_utils';

export function renderSplitPanel(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'SplitPanel'),
      m(
        'p',
        'A resizable split panel container for dividing content into adjustable sections with a draggable divider.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(
          '',
          {
            style: {
              height: '400px',
              width: '400px',
              border: 'solid 2px gray',
            },
          },
          m(
            SplitPanel,
            {
              leftHandleContent: [
                opts.leftContent && m(Button, {icon: 'Menu'}),
              ],
              drawerContent: 'Drawer Content',
              tabs:
                opts.tabs &&
                m(
                  '.pf-split-panel__tabs',
                  m(
                    Tab,
                    {active: true, hasCloseButton: opts.showCloseButtons},
                    'Foo',
                  ),
                  m(Tab, {hasCloseButton: opts.showCloseButtons}, 'Bar'),
                ),
            },
            'Main Content',
          ),
        );
      },
      initialOpts: {
        leftContent: true,
        tabs: true,
        showCloseButtons: true,
      },
    }),
  ];
}
