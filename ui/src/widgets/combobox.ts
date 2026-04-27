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
import {Popup, PopupPosition} from './popup';
import {TextInput} from './text_input';
import {FuzzyFinder, FuzzySegment} from '../base/fuzzy';
import {classNames} from '../base/classnames';
import {HotkeyGlyphs, Keycap} from './hotkey_glyphs';
import {Menu, MenuDivider, MenuItem} from './menu';

/**
 * A text input with a filterable suggestion dropdown.
 *
 * Spec:
 * - When empty, focus (click or keyboard) to open the popup and show all
 *   options.
 * - Whenver there is text in the input, show the fuzzy filtered list of options
 *   a the top of the popup, with the full list below.
 * - If the input text doesn't exactly match an option (case sensitive), show an
 *   additional option 'Use "input text"' at the bottom of the filtered list.
 * - Bluring or tabbing away from the input closes the popup.
 * - Blurring the input never changes the text value, providing an escape hatch
 *   to allow the user to choose an item that's not in the list.
 * - Whenever the user modifies the search term, the highlighted index resets to
 *   the first item in the filtered list (or the 'Use "input text"' item if
 *   there are no filtered results).
 * - Pressing the arrow keys when the popup is open moves the highlighted index
 *   up and down.
 * - Pressing enter when the popup is open replaces the input text with the
 *   highlighted item, and closes the popup - the input remains focused,
 *   allowing the user to tab to the next input in the form.
 * - Pressing escape when the popup is open closes the popup.
 *
 * Edge cases:
 * - Arrow down past the last or arrow up past the first item clamps.
 * - Pressing any key when the popup is closed opens the popup.
 */

export interface ComboboxSuggestion {
  readonly value: string;
  readonly icon?: string;
}

export interface ComboboxAttrs {
  readonly suggestions: ReadonlyArray<ComboboxSuggestion | string>;
  readonly value: string;
  readonly onChange?: (value: string) => void;
  readonly placeholder?: string;
  readonly className?: string;
  readonly icon?: string;
}

export class Combobox implements m.ClassComponent<ComboboxAttrs> {
  private isOpen = false;
  private highlightIdx = 0;

  view({attrs}: m.CVnode<ComboboxAttrs>) {
    const {
      value,
      placeholder,
      className,
      icon,
      onChange = () => {},
      suggestions,
    } = attrs;

    const selectItem = (value: string) => {
      this.isOpen = false;
      this.highlightIdx = 0;
      onChange(value);
    };

    const suggestionsNorm = suggestions.map((s) =>
      typeof s === 'string' ? {value: s} : s,
    );

    // Avoid loading the entire list if the popup is closed to save on expensive
    // unecessary fuzzy calculations.
    const options = this.isOpen
      ? buildOptionsList(value, suggestionsNorm)
      : {filtered: [], all: []};

    const allItems = options.filtered.concat(options.all);
    const totalItems = allItems.length;

    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'ArrowDown') {
        e.preventDefault();
        this.isOpen = true;
        this.highlightIdx = Math.min(this.highlightIdx + 1, totalItems - 1);
      } else if (e.key === 'ArrowUp') {
        e.preventDefault();
        this.highlightIdx = Math.max(this.highlightIdx - 1, 0);
      } else if (e.key === 'Enter') {
        e.preventDefault();
        if (this.isOpen) {
          onChange(allItems[this.highlightIdx].value);
          this.isOpen = false;
          this.highlightIdx = 0;
        } else {
          this.isOpen = true;
        }
      } else if (e.key === 'Escape') {
        this.isOpen = false;
        this.highlightIdx = 0;
      } else {
        this.isOpen = true;
      }
    };

    const trigger = m(TextInput, {
      value,
      placeholder,
      className,
      leftIcon: icon,
      onInput: (value: string) => {
        onChange(value);
        this.isOpen = true;
        this.highlightIdx = 0;
      },
      onfocus: () => {
        this.isOpen = true;
        this.highlightIdx = 0;
      },
      onblur: () => {
        this.isOpen = false;
      },
      onpointerdown: () => {
        if (!this.isOpen) {
          this.isOpen = true;
          this.highlightIdx = 0;
        }
      },
      onkeydown: onKeyDown,
    });

    const popupMenu = [
      m(
        Menu,
        {
          tabIndex: -1, // Make the menu focusable to receive blur events.
          className: 'pf-combobox__menu',
          onfocusin: () => {
            this.isOpen = true;
          },
          onkeydown: onKeyDown,
        },
        options.filtered.map((s, idx) =>
          m(
            SuggestionItem,
            {
              key: `filtered-${s.value}`,
              isHighlighted: idx === this.highlightIdx,
              icon: s.icon,
              onclick: () => selectItem(s.value),
            },
            s.content,
          ),
        ),
        m(MenuDivider),
        options.all.map((s, idx) => {
          const realIdx = idx + options.filtered.length;
          return m(
            SuggestionItem,
            {
              key: `all-${s.value}`,
              isHighlighted: realIdx === this.highlightIdx,
              icon: s.icon,
              onclick: () => selectItem(s.value),
            },
            s.content,
          );
        }),
      ),
      this.renderFooter(),
    ];

    return m(
      Popup,
      {
        trigger,
        isOpen: this.isOpen,
        onChange: (shouldOpen: boolean) => {
          if (!shouldOpen) {
            this.isOpen = false;
            this.highlightIdx = 0;
          }
        },
        position: PopupPosition.Bottom,
        closeOnEscape: true,
        closeOnOutsideClick: true,
        className: 'pf-combobox__popup',
      },
      popupMenu,
    );
  }

  private renderFooter() {
    return m('.pf-combobox__footer', [
      m(Keycap, '↑↓'),
      ' navigate ',
      m(HotkeyGlyphs, {hotkey: 'Enter'}),
      ' select ',
      m(HotkeyGlyphs, {hotkey: 'Escape'}),
      ' dismiss',
    ]);
  }
}

interface SuggestionItemAttrs extends m.Attributes {
  readonly isHighlighted?: boolean;
  readonly icon?: string;
}

function SuggestionItem(): m.Component<SuggestionItemAttrs> {
  let prevHighlighted = false;
  return {
    view({attrs, children}: m.CVnode<SuggestionItemAttrs>) {
      const {prefix, isHighlighted, className, ...htmlAttrs} = attrs;
      return m(MenuItem, {
        ...htmlAttrs,
        className: classNames(
          'pf-combobox__item',
          isHighlighted && 'pf-highlight',
          className,
        ),
        label: [prefix, children],
      });
    },
    oncreate({attrs, dom}: m.VnodeDOM<SuggestionItemAttrs>) {
      prevHighlighted = attrs.isHighlighted ?? false;
      if (attrs.isHighlighted) {
        dom.scrollIntoView({block: 'nearest'});
      }
    },
    onupdate({attrs, dom}: m.VnodeDOM<SuggestionItemAttrs>) {
      const isHighlighted = attrs.isHighlighted ?? false;
      const justHighlighted = isHighlighted && !prevHighlighted;
      prevHighlighted = isHighlighted;
      if (justHighlighted) {
        dom.scrollIntoView({block: 'nearest'});
      }
    },
  };
}

function formatSegments(segments: FuzzySegment[]): m.Children {
  return segments.map(({matching, value}) =>
    matching ? m('b', value) : value,
  );
}

function buildOptionsList(
  value: string,
  suggestions: readonly ComboboxSuggestion[],
): {
  filtered: {value: string; content: m.Children; icon?: string}[];
  all: {value: string; content: m.Children; icon?: string}[];
} {
  const filtered = buildFilteredItems(value, suggestions);
  const exact = buildExactMatchItem(value, suggestions);
  const all = buildAllItems(suggestions);

  return {
    filtered: [...filtered, ...exact],
    all,
  };
}

function buildFilteredItems(
  value: string,
  suggestions: readonly ComboboxSuggestion[],
) {
  if (value === '') {
    return [];
  } else {
    // Do the filtering and add a single list of options up the top which
    // contains the filtered suggestions list.
    const normalize = (s: ComboboxSuggestion | string) =>
      typeof s === 'string' ? {value: s} : s;
    const norm = suggestions.map(normalize);
    const fuzzy = new FuzzyFinder(norm, (s) => s.value);
    return fuzzy.find(value).map(({item, segments}) => ({
      value: item.value,
      content: formatSegments(segments),
      icon: item.icon,
    }));
  }
}

function buildExactMatchItem(
  value: string,
  suggestions: readonly ComboboxSuggestion[],
) {
  if (value === '') {
    return [];
  }

  const exactMatch = suggestions.some((s) => s.value === value);
  if (exactMatch) {
    return [];
  } else {
    return [{value, content: `Use '${value}'`}];
  }
}

function buildAllItems(suggestions: readonly ComboboxSuggestion[]) {
  return suggestions.map((s) => ({
    value: s.value,
    content: s.value,
    icon: s.icon,
  }));
}
