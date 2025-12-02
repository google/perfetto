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

/**
 * MultiselectInput - A widget for selecting multiple items from a list with
 * auto-complete.
 *
 * This widget provides a text-input-based interface for multi-selection,
 * combining typing to filter with clicking to select. Selected items appear as
 * removable chips in the input field.
 *
 * Features:
 * - Type to filter available options
 * - Click options to toggle selection
 * - Selected items shown as chips with remove buttons
 * - Keyboard navigation with arrow keys
 * - Popup automatically opens on focus, closes on blur
 *
 * User Interactions:
 * - Click anywhere in the input area to focus and open the popup
 * - Type text to filter the available options
 * - Click an option in the popup to toggle its selection
 * - Click the × on a chip to remove that selection
 * - Click outside the widget or tab away to close the popup
 *
 * Keyboard Shortcuts:
 * - Enter: Toggle selection of the currently highlighted option
 * - ArrowUp/ArrowDown: Navigate through filtered options
 * - Backspace (when input empty): Remove the last selected chip
 * - Escape: Close the popup by blurring the input
 *
 * Implementation Details:
 * - Popup state is tied to input focus (focused = open, blurred = closed)
 * - Clicking inside the popup prevents input blur via
 *   mousedown.preventDefault()
 * - All keyboard handling uses Mithril's event system (no manual event
 *   listeners)
 * - Uses controlled mode for the Popup widget (no automatic toggle on trigger
 *   click)
 */

import m from 'mithril';
import {HTMLAttrs, Intent} from './common';
import {Icon} from './icon';
import {Popup, PopupPosition} from './popup';
import {EmptyState} from './empty_state';
import {classNames} from '../base/classnames';
import {Stack} from './stack';
import {Chip} from './chip';

export interface Option {
  readonly key: string;
  readonly label: string;
}

export interface MultiselectInputAttrs extends HTMLAttrs {
  readonly options: ReadonlyArray<Option>;
  readonly selectedOptions: ReadonlyArray<string>;
  readonly onOptionAdd: (key: string) => void;
  readonly onOptionRemove: (key: string) => void;
  readonly placeholder?: string;
}

export class MultiselectInput
  implements m.ClassComponent<MultiselectInputAttrs>
{
  private currentTextValue = '';
  private selectedItemIndex = 0;
  private popupIsOpen = false;

  view({attrs}: m.CVnode<MultiselectInputAttrs>) {
    const {selectedOptions, placeholder, options, ...htmlAttrs} = attrs;

    return m(
      Popup,
      {
        className: 'pf-multiselect-input__popup',
        position: PopupPosition.Bottom,
        matchWidth: true,
        isOpen: this.popupIsOpen,
        // Disable Popup's built-in close handlers - we manage via input focus/blur
        closeOnEscape: false,
        closeOnOutsideClick: false,
        trigger: m(
          '.pf-multiselect-input',
          htmlAttrs,
          // Render the selected options as tags in the text field
          m(
            Stack,
            {orientation: 'horizontal', spacing: 'small'},
            selectedOptions.map((key) => {
              const option = options.find((o) => o.key === key);
              if (option) {
                return renderTag({
                  label: option.label,
                  onRemove: () => attrs.onOptionRemove(option.key),
                });
              } else {
                return undefined;
              }
            }),
          ),
          m('input', {
            value: this.currentTextValue,
            placeholder,
            onfocus: () => {
              this.popupIsOpen = true;
            },
            onblur: () => {
              this.popupIsOpen = false;
            },
            onkeydown: (ev: KeyboardEvent) => {
              const filteredOptions = this.filterOptions(attrs);

              if (ev.key === 'Escape') {
                // Blur the input, which will close the popup via onblur
                (ev.target as HTMLInputElement).blur();
                ev.preventDefault();
              } else if (ev.key === 'Enter') {
                if (filteredOptions.length > 0) {
                  const option = filteredOptions[this.selectedItemIndex];
                  const alreadyAdded = selectedOptions.includes(option.key);
                  if (alreadyAdded) {
                    attrs.onOptionRemove(option.key);
                  } else {
                    attrs.onOptionAdd(option.key);
                  }
                  this.currentTextValue = '';
                }
                ev.preventDefault();
              } else if (ev.key === 'ArrowUp') {
                if (filteredOptions.length > 0) {
                  this.selectedItemIndex = Math.max(
                    0,
                    this.selectedItemIndex - 1,
                  );
                }
                ev.preventDefault();
              } else if (ev.key === 'ArrowDown') {
                if (filteredOptions.length > 0) {
                  this.selectedItemIndex = Math.min(
                    filteredOptions.length - 1,
                    this.selectedItemIndex + 1,
                  );
                }
                ev.preventDefault();
              } else if (ev.key === 'Backspace') {
                if (
                  this.currentTextValue === '' &&
                  selectedOptions.length > 0
                ) {
                  attrs.onOptionRemove(
                    selectedOptions[selectedOptions.length - 1],
                  );
                  ev.preventDefault();
                }
              }
            },
            oninput: (ev: InputEvent) => {
              const el = ev.target as HTMLInputElement;
              this.currentTextValue = el.value;
              this.selectedItemIndex = 0;
            },
          }),
        ),
      },
      this.renderOptionsPopup(attrs),
    );
  }

  private renderOptionsPopup(attrs: MultiselectInputAttrs) {
    const {onOptionAdd, onOptionRemove, selectedOptions} = attrs;

    const filtered = this.filterOptions(attrs);
    if (filtered.length === 0) {
      return m(EmptyState, {title: 'No results found'});
    }

    return m(
      '.pf-multiselect-input__scroller',
      {
        onmousedown: (e: MouseEvent) => {
          // Prevent input from losing focus when clicking inside popup
          e.preventDefault();
        },
      },
      filtered.map((o, index) => {
        const alreadyAdded = selectedOptions.includes(o.key);
        return m(
          '.pf-multiselect-input__option-row',
          {
            key: o.key,
            className: classNames(
              this.selectedItemIndex === index &&
                'pf-multiselect-input__option-row--selected',
            ),
            onclick: () => {
              if (alreadyAdded) {
                onOptionRemove(o.key);
              } else {
                onOptionAdd(o.key);
              }
            },
          },
          alreadyAdded && m(Icon, {icon: 'check'}),
          o.label,
        );
      }),
    );
  }

  private filterOptions({options}: MultiselectInputAttrs) {
    return options.filter((o) => {
      return o.label
        .toLowerCase()
        .includes(this.currentTextValue.toLowerCase());
    });
  }
}

interface TagAttrs {
  readonly label: string;
  readonly onRemove?: () => void;
}

function renderTag({label, onRemove}: TagAttrs): m.Children {
  return m(Chip, {
    label,
    compact: true,
    intent: Intent.Primary,
    removable: true,
    onRemove: () => onRemove?.(),
  });
}
