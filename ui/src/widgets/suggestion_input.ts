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

// A normal text input that shows a filterable suggestion dropdown.
//
// The text box value is always respected — suggestions are passive.
// The user can accept a suggestion via arrow keys + enter, or by clicking
// one. Everything else behaves like a regular text input.
// Suggestions are fuzzy-matched and matching segments are highlighted.

export interface Suggestion {
  readonly value: string;
  // Optional content rendered before the suggestion text (e.g. an icon).
  readonly prefix?: m.Children;
}

export interface SuggestionInputAttrs {
  readonly value: string;
  readonly suggestions: ReadonlyArray<Suggestion | string>;
  readonly onChange: (value: string) => void;
  readonly placeholder?: string;
  readonly className?: string;
}

interface FilteredSuggestion {
  readonly value: string;
  readonly prefix?: m.Children;
  readonly segments: FuzzySegment[];
}

function renderSegments(segments: FuzzySegment[]): m.Children {
  return segments.map(({matching, value}) =>
    matching ? m('b', value) : value,
  );
}

// Default highlight index — 0 so Enter always picks the best match.
const HIGHLIGHT_DEFAULT = 0;

function scrollIntoViewIfNeeded(vnode: m.VnodeDOM) {
  const el = vnode.dom as HTMLElement;
  if (el.classList.contains('highlight')) {
    el.scrollIntoView({block: 'nearest'});
  }
}

export class SuggestionInput implements m.ClassComponent<SuggestionInputAttrs> {
  private isOpen = false;
  private highlightIdx = HIGHLIGHT_DEFAULT;
  private finder: FuzzyFinder<Suggestion> | undefined;
  private lastSuggestions: ReadonlyArray<Suggestion | string> | undefined;

  private ensureFinder(
    suggestions: ReadonlyArray<Suggestion | string>,
  ): FuzzyFinder<Suggestion> {
    if (this.finder && this.lastSuggestions === suggestions) {
      return this.finder;
    }
    const normalized = suggestions.map((s) =>
      typeof s === 'string' ? {value: s} : s,
    );
    this.finder = new FuzzyFinder(normalized, (s) => s.value);
    this.lastSuggestions = suggestions;
    return this.finder;
  }

  private getFiltered(
    query: string,
    suggestions: ReadonlyArray<Suggestion | string>,
  ): FilteredSuggestion[] {
    const finder = this.ensureFinder(suggestions);
    return finder.find(query).map((result) => ({
      value: result.item.value,
      prefix: result.item.prefix,
      segments: result.segments,
    }));
  }

  private renderItem(
    idx: number,
    content: m.Children,
    onSelect: () => void,
    className?: string,
  ): m.Vnode {
    return m(
      '.pf-suggestion-input__item',
      {
        class: [
          idx === this.highlightIdx ? 'highlight' : '',
          className ?? '',
        ].join(' '),
        oncreate: scrollIntoViewIfNeeded,
        onupdate: scrollIntoViewIfNeeded,
        onmousedown: (e: MouseEvent) => {
          e.preventDefault();
          onSelect();
          this.isOpen = false;
          this.highlightIdx = HIGHLIGHT_DEFAULT;
        },
        onmouseenter: () => {
          this.highlightIdx = idx;
        },
      },
      content,
    );
  }

  view({attrs}: m.CVnode<SuggestionInputAttrs>) {
    const filtered = this.getFiltered(attrs.value, attrs.suggestions);
    const hasQuery = attrs.value.length > 0;
    // "All suggestions" excludes items already shown in fuzzy matches.
    const filteredValues = new Set(filtered.map((s) => s.value));
    const all = hasQuery
      ? this.getFiltered('', attrs.suggestions).filter(
          (s) => !filteredValues.has(s.value),
        )
      : [];
    const exactMatch = filtered.some(
      (s) => s.value.toLowerCase() === attrs.value.toLowerCase(),
    );
    const showUseOption = hasQuery && !exactMatch;
    // When there's a query, show: matches + use + all suggestions section.
    // When empty, just show all suggestions (no duplication needed).
    const showAllSection = hasQuery && all.length > 0;

    // Compute index offsets for each section:
    // [0..filtered.length) = matches
    // [filtered.length] = "use" option (if shown)
    // [useOffset..useOffset+all.length) = all suggestions
    const useIdx = filtered.length;
    const allOffset = filtered.length + (showUseOption ? 1 : 0);
    const totalItems = allOffset + (showAllSection ? all.length : 0);
    const hasItems = totalItems > 0;

    // Clamp highlight index to valid range after filtering changes.
    if (totalItems > 0) {
      this.highlightIdx = Math.min(this.highlightIdx, totalItems - 1);
      this.highlightIdx = Math.max(this.highlightIdx, 0);
    }

    return m(
      Popup,
      {
        trigger: m(TextInput, {
          value: attrs.value,
          placeholder: attrs.placeholder,
          className: attrs.className,
          onInput: (value: string) => {
            attrs.onChange(value);
            this.isOpen = true;
            this.highlightIdx = HIGHLIGHT_DEFAULT;
          },
          onfocus: () => {
            this.isOpen = true;
            this.highlightIdx = HIGHLIGHT_DEFAULT;
          },
          onblur: () => {
            this.isOpen = false;
            this.highlightIdx = HIGHLIGHT_DEFAULT;
          },
          onkeydown: (e: KeyboardEvent) => {
            if (e.key === 'ArrowDown') {
              e.preventDefault();
              this.isOpen = true;
              this.highlightIdx = Math.min(
                this.highlightIdx + 1,
                totalItems - 1,
              );
            } else if (e.key === 'ArrowUp') {
              e.preventDefault();
              this.highlightIdx = Math.max(
                this.highlightIdx - 1,
                HIGHLIGHT_DEFAULT,
              );
            } else if (e.key === 'Enter') {
              e.preventDefault();
              if (
                this.highlightIdx >= 0 &&
                this.highlightIdx < filtered.length
              ) {
                attrs.onChange(filtered[this.highlightIdx].value);
              } else if (showAllSection && this.highlightIdx >= allOffset) {
                attrs.onChange(all[this.highlightIdx - allOffset].value);
              }
              // "Use" option or out of range: value already in input.
              this.highlightIdx = HIGHLIGHT_DEFAULT;
            } else if (e.key === 'Escape') {
              this.isOpen = false;
              this.highlightIdx = HIGHLIGHT_DEFAULT;
            }
          },
        }),
        isOpen: this.isOpen && hasItems,
        onChange: (shouldOpen: boolean) => {
          if (!shouldOpen) {
            this.isOpen = false;
            this.highlightIdx = HIGHLIGHT_DEFAULT;
          }
        },
        position: PopupPosition.Bottom,
        closeOnEscape: true,
        closeOnOutsideClick: true,
        className: 'pf-suggestion-input__popup',
      },
      m('.pf-suggestion-input__list', [
        ...filtered.map((suggestion, idx) =>
          this.renderItem(
            idx,
            [suggestion.prefix, m('span', renderSegments(suggestion.segments))],
            () => attrs.onChange(suggestion.value),
          ),
        ),
        showUseOption &&
          this.renderItem(
            useIdx,
            `Use '${attrs.value}'`,
            () => {},
            'pf-suggestion-input__use',
          ),
        ...(showAllSection
          ? [
              m('.pf-suggestion-input__section-label', 'All suggestions'),
              ...all.map((suggestion, idx) =>
                this.renderItem(
                  allOffset + idx,
                  [
                    suggestion.prefix,
                    m('span', renderSegments(suggestion.segments)),
                  ],
                  () => attrs.onChange(suggestion.value),
                ),
              ),
            ]
          : []),
      ]),
      m('.pf-suggestion-input__footer', [
        m('kbd', '↑↓'),
        ' navigate ',
        m('kbd', '↵'),
        ' select ',
        m('kbd', 'esc'),
        ' dismiss',
      ]),
    );
  }
}
