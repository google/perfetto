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

import * as m from 'mithril';
import {globals} from '../globals';
import {DESELECT, SELECT_ALL} from '../icons';
import {Button} from './button';
import {Checkbox} from './checkbox';
import {EmptyState} from './empty_state';
import {Popup, PopupPosition} from './popup';
import {TextInput} from './text_input';

export interface Option {
  // The ID is used to indentify this option, and is used in callbacks.
  id: string;
  // This is the name displayed and used for searching.
  name: string;
  // Whether the option is selected or not.
  checked: boolean;
}

export interface MultiSelectDiff {
  id: string;
  checked: boolean;
}

export interface MultiSelectAttrs {
  icon?: string;
  label: string;
  options: Option[];
  onChange?: (diffs: MultiSelectDiff[]) => void;
  repeatCheckedItemsAtTop?: boolean;
  showNumSelected?: boolean;
  popupPosition?: PopupPosition;
}

// A component which shows a list of items with checkboxes, allowing the user to
// select from the list which ones they want to be selected.
// Also provides search functionality.
// This component is entirely controlled and callbacks must be supplied for when
// the selected items changes, and when the search term changes.
// There is an optional boolean flag to enable repeating the selected items at
// the top of the list for easy access - defaults to false.
export class MultiSelect implements m.ClassComponent<MultiSelectAttrs> {
  private searchText: string = '';
  view({attrs}: m.CVnode<MultiSelectAttrs>) {
    const {
      icon,
      popupPosition = PopupPosition.Auto,
    } = attrs;

    return m(
        Popup,
        {
          trigger: m(Button, {label: this.labelText(attrs), icon}),
          position: popupPosition,
        },
        this.renderPopup(attrs),
    );
  }

  private labelText(attrs: MultiSelectAttrs): string {
    const {
      options,
      showNumSelected,
      label,
    } = attrs;

    if (showNumSelected) {
      const numSelected = options.filter(({checked}) => checked).length;
      return `${label} (${numSelected} selected)`;
    } else {
      return label;
    }
  }

  private renderPopup(attrs: MultiSelectAttrs) {
    const {
      options,
    } = attrs;

    const filteredItems = options.filter(({name}) => {
      return name.toLowerCase().includes(this.searchText.toLowerCase());
    });

    return m(
        '.pf-multiselect-popup',
        this.renderSearchBox(),
        this.renderListOfItems(attrs, filteredItems),
    );
  }

  private renderListOfItems(attrs: MultiSelectAttrs, options: Option[]) {
    const {
      repeatCheckedItemsAtTop,
      onChange = () => {},
    } = attrs;
    const allChecked = options.every(({checked}) => checked);
    const anyChecked = options.some(({checked}) => checked);

    if (options.length === 0) {
      return m(EmptyState, {
        header: `No results for '${this.searchText}'`,
      });
    } else {
      return [m(
          '.pf-list',
          repeatCheckedItemsAtTop && anyChecked &&
              m(
                  '.pf-multiselect-container',
                  m(
                      '.pf-multiselect-header',
                      m('span',
                        this.searchText === '' ? 'Selected' :
                                                 `Selected (Filtered)`),
                      m(Button, {
                        label: this.searchText === '' ? 'Clear All' :
                                                        'Clear Filtered',
                        icon: DESELECT,
                        minimal: true,
                        onclick: () => {
                          const diffs =
                              options.filter(({checked}) => checked)
                                  .map(({id}) => ({id, checked: false}));
                          onChange(diffs);
                          globals.rafScheduler.scheduleFullRedraw();
                        },
                        disabled: !anyChecked,
                      }),
                      ),
                  this.renderOptions(
                      attrs, options.filter(({checked}) => checked)),
                  ),
          m(
              '.pf-multiselect-container',
              m(
                  '.pf-multiselect-header',
                  m('span',
                    this.searchText === '' ? 'Options' : `Options (Filtered)`),
                  m(Button, {
                    label: this.searchText === '' ? 'Select All' :
                                                    'Select Filtered',
                    icon: SELECT_ALL,
                    minimal: true,
                    compact: true,
                    onclick: () => {
                      const diffs = options.filter(({checked}) => !checked)
                                        .map(({id}) => ({id, checked: true}));
                      onChange(diffs);
                      globals.rafScheduler.scheduleFullRedraw();
                    },
                    disabled: allChecked,
                  }),
                  m(Button, {
                    label: this.searchText === '' ? 'Clear All' :
                                                    'Clear Filtered',
                    icon: DESELECT,
                    minimal: true,
                    compact: true,
                    onclick: () => {
                      const diffs = options.filter(({checked}) => checked)
                                        .map(({id}) => ({id, checked: false}));
                      onChange(diffs);
                      globals.rafScheduler.scheduleFullRedraw();
                    },
                    disabled: !anyChecked,
                  }),
                  ),
              this.renderOptions(attrs, options),
              ),
          )];
    }
  }

  private renderSearchBox() {
    return m(
        '.pf-search-bar',
        m(TextInput, {
          oninput: (event: Event) => {
            const eventTarget = event.target as HTMLTextAreaElement;
            this.searchText = eventTarget.value;
            globals.rafScheduler.scheduleFullRedraw();
          },
          value: this.searchText,
          placeholder: 'Filter options...',
          extraClasses: 'pf-search-box',
        }),
        this.renderClearButton(),
    );
  }

  private renderClearButton() {
    if (this.searchText != '') {
      return m(Button, {
        onclick: () => {
          this.searchText = '';
          globals.rafScheduler.scheduleFullRedraw();
        },
        label: '',
        icon: 'close',
        minimal: true,
      });
    } else {
      return null;
    }
  }

  private renderOptions(attrs: MultiSelectAttrs, options: Option[]) {
    const {
      onChange = () => {},
    } = attrs;

    return options.map((item) => {
      const {checked, name, id} = item;
      return m(Checkbox, {
        label: name,
        key: id,  // Prevents transitions jumping between items when searching
        checked,
        classes: 'pf-multiselect-item',
        onchange: () => {
          onChange([{id, checked: !checked}]);
          globals.rafScheduler.scheduleFullRedraw();
        },
      });
    });
  }
}
