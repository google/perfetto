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

import {FuzzySegment} from '../base/fuzzy';
import {exists} from '../base/utils';
import {raf} from '../core/raf_scheduler';

import {classNames} from './classnames';
import {EmptyState} from './widgets/empty_state';
import {Popup} from './widgets/popup';

interface OmniboxOptionRowAttrs {
  title: FuzzySegment[];
  highlighted: boolean;
  [htmlAttrs: string]: any;
}

class OmniboxOptionRow implements m.ClassComponent<OmniboxOptionRowAttrs> {
  view({attrs}: m.Vnode<OmniboxOptionRowAttrs>): void|m.Children {
    const {title, highlighted, ...htmlAttrs} = attrs;
    return m(
        'li.pf-omnibox-option',
        {
          class: classNames(highlighted && 'pf-highlighted'),
          ...htmlAttrs,
        },
        title.map(({matching, value}) => {
          return matching ? m('b', value) : value;
        }),
    );
  }
}

export interface OmniboxOption {
  key: string;
  displayName: FuzzySegment[];
}

export interface OmniboxAttrs {
  // Omnibox text.
  value: string;

  // What to show when value is blank.
  placeholder?: string;

  // Called when the text changes.
  onInput?: (value: string, previousValue: string) => void;

  // Class or list of classes to append to the Omnibox element.
  extraClasses?: string|string[];

  // Called on close.
  onClose?: () => void;

  // Dropdown items to show.
  options?: OmniboxOption[];

  // Called when the user expresses the intent to "execute" the thing.
  onSubmit?: (value: string, mod: boolean, shift: boolean) => void;

  // Icon to show on the left.
  icon?: string;

  // When true, disable input in the bar and show a more gray appearance.
  readonly?: boolean;

  inputRef?: string;

  closeOnSubmit?: boolean;

  closeOnOutsideClick?: boolean;

  rightContent?: m.Children;
}

export class Omnibox implements m.ClassComponent<OmniboxAttrs> {
  private highlightedOptionIndex = 0;
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
      inputRef = 'omnibox',
      options,
      closeOnSubmit = false,
      rightContent,
    } = attrs;

    return m(
        Popup,
        {
          className: 'pf-popup-padded',
          onPopupMount: (dom: HTMLElement) => this.popupElement = dom,
          onPopupUnMount: (_dom: HTMLElement) => this.popupElement = undefined,
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
                  this.highlightedOptionIndex = 0;
                  onInput((e.target as HTMLInputElement).value, value);
                },
                onkeydown: (e: KeyboardEvent) => {
                  if (e.key === 'Backspace' && value === '') {
                    this.close(attrs);
                  } else if (e.key === 'Escape') {
                    e.preventDefault();
                    this.close(attrs);
                  }

                  if (options) {
                    if (e.key === 'ArrowUp') {
                      e.preventDefault();
                      this.highlightPreviousOption();
                    } else if (e.key === 'ArrowDown') {
                      e.preventDefault();
                      this.highlightNextOption(options);
                    } else if (e.key === 'Enter') {
                      e.preventDefault();

                      const option = options[this.highlightedOptionIndex];
                      if (option) {
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
        options && this.renderOptions(attrs));
  }

  private renderOptions(attrs: OmniboxAttrs): m.Children {
    const {
      onClose = () => {},
      onSubmit = () => {},
      options,
      closeOnSubmit = false,
    } = attrs;

    if (!options) return null;

    if (options.length === 0) {
      return m(EmptyState, {header: 'No matching commands...'});
    } else {
      return m(
          '.pf-omnibox-dropdown',
          options.map(({displayName, key}, index) => {
            return m(OmniboxOptionRow, {
              key,
              title: displayName,
              highlighted: index === this.highlightedOptionIndex,
              onclick: () => {
                closeOnSubmit && onClose();
                onSubmit(key, false, false);
              },
            });
          }),
      );
    }
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
    this.highlightedOptionIndex = 0;
    onClose();
  }

  private highlightPreviousOption() {
    this.highlightedOptionIndex = Math.max(0, --this.highlightedOptionIndex);
    raf.scheduleFullRedraw();
  }

  private highlightNextOption(commands: OmniboxOption[]) {
    const max = commands.length - 1;
    this.highlightedOptionIndex = Math.min(max, ++this.highlightedOptionIndex);
    raf.scheduleFullRedraw();
  }
}
