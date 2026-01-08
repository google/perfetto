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
import {SplitPanel} from '../../../widgets/split_panel';
import {renderWidgetShowcase} from '../widgets_page_utils';

export function renderSplitPanel(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'SplitPanel'),
      m(
        'p',
        'A simple resizable split panel with a draggable handle. Supports both horizontal and vertical layouts, with percentage or fixed-pixel sizing modes.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(
          '',
          {
            style: {
              height: '300px',
              width: '500px',
              border: '1px solid var(--pf-color-border)',
            },
          },
          m(SplitPanel, {
            direction: opts.vertical ? 'vertical' : 'horizontal',
            split: opts.fixed
              ? {fixed: {panel: 'first', size: 150}}
              : {percent: 50},
            minSize: 50,
            firstPanel: m(
              '',
              {
                style: {
                  padding: '8px',
                  background: 'var(--pf-color-background)',
                },
              },
              'First Panel',
            ),
            secondPanel: m(
              '',
              {
                style: {
                  padding: '8px',
                  background: 'var(--pf-color-background-secondary)',
                },
              },
              'Second Panel',
            ),
          }),
        );
      },
      initialOpts: {
        vertical: false,
        fixed: false,
      },
    }),
  ];
}
