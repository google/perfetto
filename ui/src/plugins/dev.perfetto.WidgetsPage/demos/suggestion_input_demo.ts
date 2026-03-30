// Copyright (C) 2026 The Android Open Source Project
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
import {SuggestionInput} from '../../../widgets/suggestion_input';
import {renderWidgetShowcase} from '../widgets_page_utils';

const SAMPLE_SUGGESTIONS = [
  'slice',
  'sched_slice',
  'thread_state',
  'thread',
  'process',
  'counter',
  'android_logs',
  'cpu_counter_track',
];

let selectedValue = '';

export function renderSuggestionInput(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'SuggestionInput'),
      m(
        'p',
        'A normal text input that shows a filterable suggestion dropdown. ' +
          'The text box value is always respected — suggestions are passive. ' +
          'Accept a suggestion via arrow keys + enter, or by clicking one.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: () =>
        m(SuggestionInput, {
          value: selectedValue,
          suggestions: SAMPLE_SUGGESTIONS,
          placeholder: 'table name',
          onChange: (value: string) => {
            selectedValue = value;
          },
        }),
      initialOpts: {},
    }),
  ];
}
