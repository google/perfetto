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
import {SegmentedButtons} from '../../../widgets/segmented_buttons';
import {renderWidgetShowcase} from '../widgets_page_utils';

function SegmentedButtonsDemo() {
  let selectedIdx = 0;
  return {
    view: ({attrs}: m.Vnode<{disabled: boolean}>) => {
      return m(SegmentedButtons, {
        ...attrs,
        options: [{label: 'Yes'}, {label: 'Maybe'}, {label: 'No'}],
        selectedOption: selectedIdx,
        onOptionSelected: (num) => {
          selectedIdx = num;
        },
      });
    },
  };
}

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
        return m(SegmentedButtonsDemo, opts);
      },
      initialOpts: {
        disabled: false,
      },
    }),
  ];
}
