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
import {renderWidgetShowcase} from '../widgets_page_utils';
import {
  MultiSelect,
  MultiSelectDiff,
  PopupMultiSelect,
} from '../../../widgets/multiselect';
import {PopupPosition} from '../../../widgets/popup';
import {Icons} from '../../../base/semantic_icons';

const options: {[key: string]: boolean} = {
  foobar: false,
  foo: false,
  bar: false,
  baz: false,
  qux: false,
  quux: false,
  corge: false,
  grault: false,
  garply: false,
  waldo: false,
  fred: false,
  plugh: false,
  xyzzy: false,
  thud: false,
};

export function renderMultiselect() {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Multiselect'),
      m(
        'p',
        'A dropdown component for selecting multiple options from a list with checkboxes and search functionality.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: ({...rest}) =>
        m(MultiSelect, {
          options: Object.entries(options).map(([key, value]) => {
            return {
              id: key,
              name: key,
              checked: value,
            };
          }),
          onChange: (diffs: MultiSelectDiff[]) => {
            diffs.forEach(({id, checked}) => {
              options[id] = checked;
            });
          },
          ...rest,
        }),
      initialOpts: {
        repeatCheckedItemsAtTop: false,
        fixedSize: false,
      },
    }),
    renderWidgetShowcase({
      renderWidget: ({icon, ...rest}) =>
        m(PopupMultiSelect, {
          options: Object.entries(options).map(([key, value]) => {
            return {
              id: key,
              name: key,
              checked: value,
            };
          }),
          position: PopupPosition.Top,
          label: 'Multi Select',
          icon: icon && Icons.LibraryAddCheck,
          onChange: (diffs: MultiSelectDiff[]) => {
            diffs.forEach(({id, checked}) => {
              options[id] = checked;
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
    renderWidgetShowcase({
      renderWidget: () => {
        return m(MultiselectInputDemo);
      },
    }),
  ];
}

export function MultiselectInputDemo() {
  let selectedOptions: string[] = [];
  const options = [
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
  ];

  return {
    view() {
      return m(MultiselectInput, {
        options: options.map((o) => ({key: o, label: o})),
        selectedOptions,
        onOptionAdd: (key) => selectedOptions.push(key),
        onOptionRemove: (key) => {
          selectedOptions = selectedOptions.filter((x) => x !== key);
        },
      });
    },
  };
}
