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
 * TagInput - A widget for selecting tag:value pairs with auto-complete.
 *
 * This widget provides a text-input-based interface for creating tag:value
 * selections. When typing, it intelligently shows either tag suggestions
 * (before the colon) or value suggestions (after the colon).
 *
 * Features:
 * - Type "tag:" to see available tags
 * - Type "tag:val" to see available values for that tag
 * - Selected tag:value pairs shown as chips with remove buttons
 * - Keyboard navigation with arrow keys
 * - Popup automatically opens on focus, closes on blur
 *
 * User Interactions:
 * - Click anywhere in the input area to focus and open the popup
 * - Type text to filter available tags or values
 * - Click an option in the popup to select it
 * - Click the × on a chip to remove that tag:value pair
 * - Click outside the widget or tab away to close the popup
 *
 * Keyboard Shortcuts:
 * - Enter: Select the currently highlighted option
 * - ArrowUp/ArrowDown: Navigate through filtered options
 * - Backspace (when input empty): Remove the last selected chip
 * - Escape: Close the popup by blurring the input
 *
 * Implementation Details:
 * - Popup state is tied to input focus (focused = open, blurred = closed)
 * - Clicking inside the popup prevents input blur via
 *   mousedown.preventDefault()
 * - Parses input to detect tag name vs tag value based on colon position
 * - All keyboard handling uses Mithril's event system
 */

import m from 'mithril';
import {HTMLAttrs, Intent} from './common';
import {Popup, PopupPosition} from './popup';
import {EmptyState} from './empty_state';
import {classNames} from '../base/classnames';
import {Stack} from './stack';
import {Chip} from './chip';

export interface TagValue {
  readonly key: string;
  readonly label?: string;
}

export interface TagDefinition {
  readonly key: string;
  readonly label?: string;
  readonly values?: ReadonlyArray<TagValue>; // Optional - if missing, tag is freeform
  readonly freeform?: boolean; // Explicitly mark as accepting freeform text
  readonly isDefault?: boolean; // Mark as the default tag when typing without colon
}

export interface SelectedTag {
  readonly tagKey: string;
  readonly valueKey: string;
}

export interface FilterInputAttrs extends HTMLAttrs {
  readonly tags: ReadonlyArray<TagDefinition>;
  readonly selectedTags: ReadonlyArray<SelectedTag>;
  readonly onTagAdd: (tag: SelectedTag) => void;
  readonly onTagRemove: (tag: SelectedTag) => void;
  readonly placeholder?: string;
  readonly onTextChange?: (text: string) => void;
}

interface FilteredOption {
  readonly key: string;
  readonly label: string;
  readonly fullText?: string; // For tag:value completion
}

export class FilterInput implements m.ClassComponent<FilterInputAttrs> {
  private currentTextValue = '';
  private selectedItemIndex = 0;
  private popupIsOpen = false;

  view({attrs}: m.CVnode<FilterInputAttrs>) {
    const {selectedTags, placeholder, tags, ...htmlAttrs} = attrs;

    return m(
      Popup,
      {
        className: 'pf-filter-input__popup',
        position: PopupPosition.Top,
        matchWidth: true,
        isOpen: this.popupIsOpen,
        closeOnEscape: false,
        closeOnOutsideClick: false,
        trigger: m(
          '.pf-filter-input',
          htmlAttrs,
          m(
            Stack,
            {orientation: 'horizontal', wrap: true},
            selectedTags.map((selectedTag) => {
              const tag = tags.find((t) => t.key === selectedTag.tagKey);
              if (tag) {
                // For freeform tags, use valueKey directly
                if (tag.freeform || !tag.values || tag.values.length === 0) {
                  return this.renderTagChip({
                    tagLabel: tag.label ?? tag.key,
                    valueLabel: selectedTag.valueKey,
                    onRemove: () => attrs.onTagRemove(selectedTag),
                  });
                }

                // For strict tags, look up value in values array
                const value = tag.values.find(
                  (v) => v.key === selectedTag.valueKey,
                );
                if (value) {
                  return this.renderTagChip({
                    tagLabel: tag.label ?? tag.key,
                    valueLabel: value.label ?? value.key,
                    onRemove: () => attrs.onTagRemove(selectedTag),
                  });
                }
              }
              return undefined;
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
              const filteredOptions = this.getFilteredOptions(attrs);

              if (ev.key === 'Escape') {
                (ev.target as HTMLInputElement).blur();
                ev.preventDefault();
              } else if (ev.key === 'Enter') {
                if (filteredOptions.length > 0) {
                  const option = filteredOptions[this.selectedItemIndex];
                  this.handleOptionSelect(option, attrs);
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
                if (this.currentTextValue === '' && selectedTags.length > 0) {
                  // Convert the last chip back to text for editing
                  const lastTag = selectedTags[selectedTags.length - 1];
                  const tag = attrs.tags.find((t) => t.key === lastTag.tagKey);
                  if (tag) {
                    // Populate input with tag:value format
                    const tagLabel = tag.key;
                    const valueLabel = lastTag.valueKey;
                    this.currentTextValue = `${tagLabel}:${valueLabel}`;
                    this.selectedItemIndex = 0;

                    // Remove the tag
                    attrs.onTagRemove(lastTag);
                  }
                  ev.preventDefault();
                }
              }
            },
            oninput: (ev: InputEvent) => {
              const el = ev.target as HTMLInputElement;
              this.currentTextValue = el.value;
              this.selectedItemIndex = 0;

              // Notify parent of text changes
              attrs.onTextChange?.(el.value);
            },
          }),
        ),
      },
      this.renderOptionsPopup(attrs),
    );
  }

  private handleOptionSelect(
    option: FilteredOption,
    attrs: FilterInputAttrs,
  ): void {
    // If this is the empty placeholder option, do nothing
    if (option.key === '__empty__') {
      return;
    }

    // If this is the default tag option, use the default tag
    if (option.key === '__default__') {
      const defaultTag = attrs.tags.find((t) => t.isDefault);
      if (defaultTag) {
        const text = this.currentTextValue.trim();
        if (text) {
          attrs.onTagAdd({
            tagKey: defaultTag.key,
            valueKey: text,
          });
          this.currentTextValue = '';
          this.selectedItemIndex = 0;
        }
      }
      return;
    }

    // If fullText is present, use it to complete the input
    if (option.fullText) {
      this.currentTextValue = option.fullText;
      this.selectedItemIndex = 0;
      return;
    }

    // Otherwise, this is a value selection, so add the tag
    const colonIdx = this.currentTextValue.indexOf(':');
    if (colonIdx !== -1) {
      const tagPart = this.currentTextValue.substring(0, colonIdx);
      const tag = attrs.tags.find(
        (t) =>
          t.key.toLowerCase() === tagPart.toLowerCase() ||
          t.label?.toLowerCase() === tagPart.toLowerCase(),
      );

      if (tag) {
        attrs.onTagAdd({
          tagKey: tag.key,
          valueKey: option.key,
        });
        this.currentTextValue = '';
        this.selectedItemIndex = 0;
      }
    }
  }

  private renderOptionsPopup(attrs: FilterInputAttrs) {
    const filtered = this.getFilteredOptions(attrs);
    if (filtered.length === 0) {
      return m(EmptyState, {title: 'No results found', icon: 'search_off'});
    }

    return m(
      '.pf-filter-input__scroller',
      {
        onmousedown: (e: MouseEvent) => {
          e.preventDefault();
        },
      },
      filtered.map((o, index) => {
        return m(
          '.pf-filter-input__option-row',
          {
            key: o.key,
            className: classNames(
              this.selectedItemIndex === index &&
                'pf-filter-input__option-row--selected',
            ),
            onclick: () => {
              this.handleOptionSelect(o, attrs);
            },
          },
          o.label,
        );
      }),
    );
  }

  private getFilteredOptions(attrs: FilterInputAttrs): FilteredOption[] {
    const colonIdx = this.currentTextValue.indexOf(':');

    if (colonIdx === -1) {
      // Before colon - show default tag option first (if exists), then tag suggestions
      const searchText = this.currentTextValue.toLowerCase();
      const options: FilteredOption[] = [];

      // Add default tag option if there's text and a default tag exists
      const defaultTag = attrs.tags.find((t) => t.isDefault);
      if (this.currentTextValue.trim() && defaultTag) {
        const defaultLabel = defaultTag.label ?? defaultTag.key;
        options.push({
          key: '__default__',
          label: `${defaultLabel}: "${this.currentTextValue}"`,
        });
      }

      // Add filtered tag suggestions (excluding default from the list)
      const tagOptions = attrs.tags
        .filter((t) => {
          const label = t.label ?? t.key;
          return (
            t.key.toLowerCase().includes(searchText) ||
            label.toLowerCase().includes(searchText)
          );
        })
        .map((t) => ({
          key: t.key,
          label: `${t.label ?? t.key}:`, // Add colon to indicate it's a tag
          fullText: `${t.key}:`,
        }));

      options.push(...tagOptions);
      return options;
    } else {
      // After colon - show value suggestions for the specified tag
      const tagPart = this.currentTextValue.substring(0, colonIdx);
      const valuePart = this.currentTextValue.substring(colonIdx + 1).trim();

      const tag = attrs.tags.find((t) => {
        const label = t.label ?? t.key;
        return (
          t.key.toLowerCase() === tagPart.toLowerCase() ||
          label.toLowerCase() === tagPart.toLowerCase()
        );
      });

      if (!tag) {
        return [];
      }

      // For freeform tags, allow any text
      if (tag.freeform || !tag.values || tag.values.length === 0) {
        if (valuePart.trim()) {
          return [
            {
              key: valuePart.trim(),
              label: `${tag.label ?? tag.key}:${valuePart.trim()}`,
            },
          ];
        }
        // Show placeholder when value is empty
        return [
          {
            key: '__empty__',
            label: `${tag.label ?? tag.key}:<freeform value>`,
          },
        ];
      }

      // For strict tags, filter from values array
      const searchText = valuePart.toLowerCase();
      return tag.values
        .filter((v) => {
          const label = v.label ?? v.key;
          return (
            v.key.toLowerCase().includes(searchText) ||
            label.toLowerCase().includes(searchText)
          );
        })
        .map((v) => ({
          key: v.key,
          label: v.label ?? v.key,
        }));
    }
  }

  private renderTagChip(params: {
    tagLabel: string;
    valueLabel: string;
    onRemove: () => void;
  }): m.Children {
    return m(Chip, {
      label: `${params.tagLabel}:${params.valueLabel}`,
      compact: true,
      intent: Intent.Primary,
      removable: true,
      onRemove: params.onRemove,
    });
  }
}
