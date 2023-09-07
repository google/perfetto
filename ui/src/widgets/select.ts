// Copyright (C) 2023 The Android Open Source Project
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

import {exists} from '../base/utils';

import {Menu, MenuItem} from './menu';
import {scheduleFullRedraw} from './raf';
import {TextInput} from './text_input';

export interface SelectAttrs {
  disabled?: boolean;
  // Whether to show a search box. Defaults to false.
  filterable?: boolean;
  [htmlAttrs: string]: any;
}

export class Select implements m.ClassComponent<SelectAttrs> {
  view({attrs, children}: m.CVnode<SelectAttrs>) {
    const {disabled = false, ...htmlAttrs} = attrs;

    return m(
        'select.pf-select' + (disabled ? '[disabled]' : ''),
        htmlAttrs,
        children);
  }
}

export interface FilterableSelectAttrs extends SelectAttrs {
  // The values to show in the select.
  values: string[];
  // Called when the user selects an option.
  onSelected: (value: string) => void;
  // If set, only the first maxDisplayedItems will be shown.
  maxDisplayedItems?: number;
  // Whether the input field should be focused when the widget is created.
  autofocusInput?: boolean;
}

// A select widget with a search box, allowing the user to filter the options.
export class FilterableSelect implements
    m.ClassComponent<FilterableSelectAttrs> {
  searchText = '';

  view({attrs}: m.CVnode<FilterableSelectAttrs>) {
    const filteredValues = attrs.values.filter((name) => {
      return name.toLowerCase().includes(this.searchText.toLowerCase());
    });

    const displayedValues = attrs.maxDisplayedItems === undefined ?
        filteredValues :
        filteredValues.slice(0, attrs.maxDisplayedItems);

    const extraItems = exists(attrs.maxDisplayedItems) &&
        Math.max(0, filteredValues.length - attrs.maxDisplayedItems);

    // TODO(altimin): when the user presses enter and there is only one item,
    // select the first one.
    // MAYBE(altimin): when the user presses enter and there are multiple items,
    // select the first one.
    return m(
        'div',
        m('.pf-search-bar',
          m(TextInput, {
            oninput: (event: Event) => {
              const eventTarget = event.target as HTMLTextAreaElement;
              this.searchText = eventTarget.value;
              scheduleFullRedraw();
            },
            onload: (event: Event) => {
              if (!attrs.autofocusInput) return;
              const eventTarget = event.target as HTMLTextAreaElement;
              eventTarget.focus();
            },
            value: this.searchText,
            placeholder: 'Filter...',
            extraClasses: 'pf-search-box',
          }),
          m(Menu,
            ...displayedValues.map((value) => m(MenuItem, {
                                     label: value,
                                     onclick: () => attrs.onSelected(value),
                                   })),
            extraItems ? m('i', `+${extraItems} more`) : null)));
  }
}
