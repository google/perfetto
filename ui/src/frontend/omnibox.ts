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
import {classNames} from '../base/classnames';
import {FuzzySegment} from '../base/fuzzy';
import {isString} from '../base/object_utils';
import {exists} from '../base/utils';
import {raf} from '../core/raf_scheduler';
import {EmptyState} from '../widgets/empty_state';
import {KeycapGlyph} from '../widgets/hotkey_glyphs';
import {Popup} from '../widgets/popup';

interface OmniboxOptionRowAttrs {
  // Human readable display name for the option.
  // This can either be a simple string, or a list of fuzzy segments in which
  // case highlighting will be applied to the matching segments.
  displayName: FuzzySegment[] | string;

  // Highlight this option.
  highlighted: boolean;

  // Arbitrary components to put on the right hand side of the option.
  rightContent?: m.Children;

  // Some tag to place on the right (to the left of the right content).
  label?: string;

  // Additional attrs forwarded to the underlying element.
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  [htmlAttrs: string]: any;
}

class OmniboxOptionRow implements m.ClassComponent<OmniboxOptionRowAttrs> {
  private highlightedBefore = false;

  view({attrs}: m.Vnode<OmniboxOptionRowAttrs>): void | m.Children {
    const {displayName, highlighted, rightContent, label, ...htmlAttrs} = attrs;
    return m(
      'li',
      {
        class: classNames(highlighted && 'pf-highlighted'),
        ...htmlAttrs,
      },
      m('span.pf-title', this.renderTitle(displayName)),
      label && m('span.pf-tag', label),
      rightContent,
    );
  }

  private renderTitle(title: FuzzySegment[] | string): m.Children {
    if (isString(title)) {
      return title;
    } else {
      return title.map(({matching, value}) => {
        return matching ? m('b', value) : value;
      });
    }
  }

  onupdate({attrs, dom}: m.VnodeDOM<OmniboxOptionRowAttrs, this>) {
    if (this.highlightedBefore !== attrs.highlighted) {
      if (attrs.highlighted) {
        dom.scrollIntoView({block: 'nearest'});
      }
      this.highlightedBefore = attrs.highlighted;
    }
  }
}

// Omnibox option.
export interface OmniboxOption {
  // The value to place into the omnibox. This is what's returned in onSubmit.
  key: string;

  // Display name provided as a string or a list of fuzzy segments to enable
  // fuzzy match highlighting.
  displayName: FuzzySegment[] | string;

  // Some tag to place on the right (to the left of the right content).
  tag?: string;

  // Arbitrary components to put on the right hand side of the option.
  rightContent?: m.Children;
}

export interface OmniboxAttrs {
  // Current value of the omnibox input.
  value: string;

  // What to show when value is blank.
  placeholder?: string;

  // Called when the text changes.
  onInput?: (value: string, previousValue: string) => void;

  // Class or list of classes to append to the Omnibox element.
  extraClasses?: string | string[];

  // Called on close.
  onClose?: () => void;

  // Dropdown items to show. If none are supplied, the omnibox runs in free text
  // mode, where anyt text can be input. Otherwise, onSubmit will always be
  // called with one of the options.
  // Options are provided in groups called categories. If the category has a
  // name the name will be listed at the top of the group rendered with a little
  // divider as well.
  options?: OmniboxOption[];

  // Called when the user expresses the intent to "execute" the thing.
  onSubmit?: (value: string, mod: boolean, shift: boolean) => void;

  // Called when the user hits backspace when the field is empty.
  onGoBack?: () => void;

  // When true, disable and grey-out the omnibox's input.
  readonly?: boolean;

  // Ref to use on the input - useful for extracing this element from the DOM.
  inputRef?: string;

  // Whether to close when the user presses Enter. Default = false.
  closeOnSubmit?: boolean;

  // Whether to close the omnibox (i.e. call the |onClose| handler) when we
  // click outside the omnibox or its dropdown. Default = false.
  closeOnOutsideClick?: boolean;

  // Some content to place into the right hand side of the after the input.
  rightContent?: m.Children;

  // If we have options, this value indicates the index of the option which
  // is currently highlighted.
  selectedOptionIndex?: number;

  // Callback for when the user pressed up/down, expressing a desire to change
  // the |selectedOptionIndex|.
  onSelectedOptionChanged?: (index: number) => void;
}

export class Omnibox implements m.ClassComponent<OmniboxAttrs> {
  private popupElement?: HTMLElement;
  private dom?: Element;
  private attrs?: OmniboxAttrs;

  view({attrs}: m.Vnode<OmniboxAttrs>): m.Children {
    const {
      value,
      placeholder,
      extraClasses,
      onInput = () => {},
      onSubmit = () => {},
      onGoBack = () => {},
      inputRef = 'omnibox',
      options,
      closeOnSubmit = false,
      rightContent,
      selectedOptionIndex = 0,
    } = attrs;

    return m(
      Popup,
      {
        onPopupMount: (dom: HTMLElement) => (this.popupElement = dom),
        onPopupUnMount: (_dom: HTMLElement) => (this.popupElement = undefined),
        isOpen: exists(options),
        showArrow: false,
        matchWidth: true,
        offset: 2,
        trigger: m(
          '.omnibox',
          {
            class: classNames(extraClasses),
          },
          m('input', {
            ref: inputRef,
            value,
            placeholder,
            oninput: (e: Event) => {
              onInput((e.target as HTMLInputElement).value, value);
            },
            onkeydown: (e: KeyboardEvent) => {
              if (e.key === 'Backspace' && value === '') {
                onGoBack();
              } else if (e.key === 'Escape') {
                e.preventDefault();
                this.close(attrs);
              }

              if (options) {
                if (e.key === 'ArrowUp') {
                  e.preventDefault();
                  this.highlightPreviousOption(attrs);
                } else if (e.key === 'ArrowDown') {
                  e.preventDefault();
                  this.highlightNextOption(attrs);
                } else if (e.key === 'Enter') {
                  e.preventDefault();

                  const option = options[selectedOptionIndex];
                  // Return values from indexing arrays can be undefined.
                  // We should enable noUncheckedIndexedAccess in
                  // tsconfig.json.
                  /* eslint-disable
                      @typescript-eslint/strict-boolean-expressions */
                  if (option) {
                    /* eslint-enable */
                    closeOnSubmit && this.close(attrs);

                    const mod = e.metaKey || e.ctrlKey;
                    const shift = e.shiftKey;
                    onSubmit(option.key, mod, shift);
                  }
                }
              } else {
                if (e.key === 'Enter') {
                  e.preventDefault();

                  closeOnSubmit && this.close(attrs);

                  const mod = e.metaKey || e.ctrlKey;
                  const shift = e.shiftKey;
                  onSubmit(value, mod, shift);
                }
              }
            },
          }),
          rightContent,
        ),
      },
      options && this.renderDropdown(attrs),
    );
  }

  private renderDropdown(attrs: OmniboxAttrs): m.Children {
    const {options} = attrs;

    if (!options) return null;

    if (options.length === 0) {
      return m(EmptyState, {title: 'No matching options...'});
    } else {
      return m(
        '.pf-omnibox-dropdown',
        this.renderOptionsContainer(attrs, options),
        this.renderFooter(),
      );
    }
  }

  private renderFooter() {
    return m(
      '.pf-omnibox-dropdown-footer',
      m(
        'section',
        m(KeycapGlyph, {keyValue: 'ArrowUp'}),
        m(KeycapGlyph, {keyValue: 'ArrowDown'}),
        'to navigate',
      ),
      m('section', m(KeycapGlyph, {keyValue: 'Enter'}), 'to use'),
      m('section', m(KeycapGlyph, {keyValue: 'Escape'}), 'to dismiss'),
    );
  }

  private renderOptionsContainer(
    attrs: OmniboxAttrs,
    options: OmniboxOption[],
  ): m.Children {
    const {
      onClose = () => {},
      onSubmit = () => {},
      closeOnSubmit = false,
      selectedOptionIndex,
    } = attrs;

    const opts = options.map(({displayName, key, rightContent, tag}, index) => {
      return m(OmniboxOptionRow, {
        key,
        label: tag,
        displayName: displayName,
        highlighted: index === selectedOptionIndex,
        onclick: () => {
          closeOnSubmit && onClose();
          onSubmit(key, false, false);
        },
        rightContent,
      });
    });

    return m('ul.pf-omnibox-options-container', opts);
  }

  oncreate({attrs, dom}: m.VnodeDOM<OmniboxAttrs, this>) {
    this.attrs = attrs;
    this.dom = dom;
    const {closeOnOutsideClick} = attrs;
    if (closeOnOutsideClick) {
      document.addEventListener('mousedown', this.onMouseDown);
    }
  }

  onupdate({attrs, dom}: m.VnodeDOM<OmniboxAttrs, this>) {
    this.attrs = attrs;
    this.dom = dom;
    const {closeOnOutsideClick} = attrs;
    if (closeOnOutsideClick) {
      document.addEventListener('mousedown', this.onMouseDown);
    } else {
      document.removeEventListener('mousedown', this.onMouseDown);
    }
  }

  onremove(_: m.VnodeDOM<OmniboxAttrs, this>) {
    this.attrs = undefined;
    this.dom = undefined;
    document.removeEventListener('mousedown', this.onMouseDown);
  }

  private onMouseDown = (e: Event) => {
    // Don't close if the click was within ourselves or our popup.
    if (e.target instanceof Node) {
      if (this.popupElement && this.popupElement.contains(e.target)) {
        return;
      }
      if (this.dom && this.dom.contains(e.target)) return;
    }
    if (this.attrs) {
      this.close(this.attrs);
    }
  };

  private close(attrs: OmniboxAttrs): void {
    const {onClose = () => {}} = attrs;
    raf.scheduleFullRedraw();
    onClose();
  }

  private highlightPreviousOption(attrs: OmniboxAttrs) {
    const {selectedOptionIndex = 0, onSelectedOptionChanged = () => {}} = attrs;

    onSelectedOptionChanged(Math.max(0, selectedOptionIndex - 1));
  }

  private highlightNextOption(attrs: OmniboxAttrs) {
    const {
      selectedOptionIndex = 0,
      onSelectedOptionChanged = () => {},
      options = [],
    } = attrs;

    const max = options.length - 1;
    onSelectedOptionChanged(Math.min(max, selectedOptionIndex + 1));
  }
}
