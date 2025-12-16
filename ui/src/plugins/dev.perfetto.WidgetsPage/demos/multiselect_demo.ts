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
import {MultiselectInput} from '../../../widgets/multiselect_input';
import {renderDocSection, renderWidgetShowcase} from '../widgets_page_utils';
import {
  MultiSelect,
  MultiSelectDiff,
  PopupMultiSelect,
} from '../../../widgets/multiselect';
import {PopupPosition} from '../../../widgets/popup';
import {Icons} from '../../../base/semantic_icons';

const availableOptions: ReadonlyArray<string> = [
  'foo',
  'bar',
  'baz',
  'qux',
  'quux',
  'corge',
  'grault',
  'garply',
  'waldo',
  'fred',
  'plugh',
  'xyzzy',
  'thud',
  'a really really long option to test overflow and wrapping handling',
];
let selectedOptions: string[] = ['foo', 'qux', 'grault'];

export function renderMultiselect() {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Multiselect'),
      m(
        'p',
        'A component for selecting multiple options from a list with checkboxes and search functionality.',
      ),
    ),

    renderWidgetShowcase({
      renderWidget: ({...rest}) =>
        m(MultiSelect, {
          options: availableOptions.map((value) => {
            return {
              id: value,
              name: value,
              checked: selectedOptions.includes(value),
            };
          }),
          onChange: (diffs: MultiSelectDiff[]) => {
            diffs.forEach(({id, checked}) => {
              selectedOptions = selectedOptions.filter((x) => x !== id);
              if (checked) {
                selectedOptions.push(id);
              }
            });
          },
          ...rest,
        }),
      initialOpts: {
        repeatCheckedItemsAtTop: false,
        fixedSize: false,
      },
    }),

    renderDocSection('PopupMultiSelect', [
      m('p', 'A multiselect component inside a popup.'),
    ]),

    renderWidgetShowcase({
      renderWidget: ({icon, ...rest}) =>
        m(PopupMultiSelect, {
          options: availableOptions.map((value) => {
            return {
              id: value,
              name: value,
              checked: selectedOptions.includes(value),
            };
          }),
          position: PopupPosition.Top,
          label: 'Multi Select',
          icon: icon && Icons.LibraryAddCheck,
          onChange: (diffs: MultiSelectDiff[]) => {
            diffs.forEach(({id, checked}) => {
              selectedOptions = selectedOptions.filter((x) => x !== id);
              if (checked) {
                selectedOptions.push(id);
              }
            });
          },
          ...rest,
        }),
      initialOpts: {
        icon: true,
        showNumSelected: true,
        repeatCheckedItemsAtTop: false,
      },
    }),

    renderDocSection('MultiselectInput', [
      m(
        'p',
        'A multiselect input with a dropdown of options and fuzzy search.',
      ),
    ]),

    renderWidgetShowcase({
      renderWidget: () => {
        return m(MultiselectInput, {
          options: availableOptions.map((o) => ({key: o, label: o})),
          selectedOptions,
          onOptionAdd: (key) => {
            selectedOptions.filter((x) => x !== key);
            selectedOptions.push(key);
          },
          onOptionRemove: (key) => {
            selectedOptions = selectedOptions.filter((x) => x !== key);
          },
        });
      },
    }),
  ];
}
