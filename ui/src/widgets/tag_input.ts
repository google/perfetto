// Copyright (C) 2024 The Android Open Source Project
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
import {HTMLFocusableAttrs} from './common';
import {Icon} from './icon';
import {findRef} from '../base/dom_utils';

export interface TagInputAttrs extends HTMLFocusableAttrs {
  value?: string;
  onChange?: (text: string) => void;
  tags: ReadonlyArray<string>;
  onTagAdd: (text: string) => void;
  onTagRemove: (index: number) => void;
  placeholder?: string;
}

const INPUT_REF = 'input';

/**
 * TagInput displays Tag elements inside an input, followed by an interactive
 * text input. The container is styled to look like a TextInput, but the actual
 * editable element appears after the last tag. Clicking anywhere on the
 * container will focus the text input.
 *
 * To use this widget, the user must provide the tags as a list of strings, and
 * provide callbacks which are called when the user modifies the list of tags,
 * either adding a new tag by typing and pressing enter, or removing a tag by
 * clicking the close button on a tag.
 *
 * The text value can be optionally be controlled, which allows access to this
 * value from outside the widget.
 *
 * Uncontrolled example:
 *
 * In this example, we only have access to the list of tags from outside.
 *
 * ```
 * const tags = [];
 *
 * m(TagInput, {
 *   tags,
 *   onTagAdd: (tag) => tags.push(tag),
 *   onTagRemove: (index) => tags.splice(index),
 * });
 * ```
 *
 * Controlled example:
 *
 * In this example we have complete control over the value in the text field.
 *
 * ```
 * const tags = [];
 * let value = '';
 *
 * m(TagInput, {
 *   tags,
 *   onTagAdd: (tag) => {
 *     tags.push(tag);
 *     value = ''; // The value is controlled so we must manually clear it here
 *   },
 *   onTagRemove: (index) => tags.splice(index),
 *   value,
 *   onChange: (x) => value = x,
 * });
 * ```
 *
 */

export class TagInput implements m.ClassComponent<TagInputAttrs> {
  view({attrs}: m.CVnode<TagInputAttrs>) {
    const {
      value,
      onChange,
      tags,
      onTagAdd,
      onTagRemove,
      onfocus,
      onblur,
      placeholder,
      ...htmlAttrs
    } = attrs;

    const valueIsControlled = value !== undefined;

    return m(
      '.pf-tag-input',
      {
        onclick: (ev: PointerEvent) => {
          const target = ev.currentTarget as HTMLElement;
          const inputElement = findRef(target, INPUT_REF);
          if (inputElement) {
            (inputElement as HTMLInputElement).focus();
          }
        },
        ...htmlAttrs,
      },
      tags.map((tag, index) => renderTag(tag, () => onTagRemove(index))),
      m('input', {
        ref: INPUT_REF,
        value,
        placeholder,
        onkeydown: (ev: KeyboardEvent) => {
          if (ev.key === 'Enter') {
            const el = ev.target as HTMLInputElement;
            if (el.value.trim() !== '') {
              onTagAdd(el.value);
              if (!valueIsControlled) {
                el.value = '';
              }
            }
          } else if (ev.key === 'Backspace') {
            const el = ev.target as HTMLInputElement;
            if (el.value !== '') return;
            if (tags.length === 0) return;

            const lastTagIndex = tags.length - 1;
            onTagRemove(lastTagIndex);
          }
        },
        oninput: (ev: InputEvent) => {
          const el = ev.target as HTMLInputElement;
          onChange?.(el.value);
        },
        onfocus,
        onblur,
      }),
    );
  }
}

function renderTag(text: string, onRemove: () => void): m.Children {
  return m(
    'span.pf-tag',
    text,
    m(Icon, {
      icon: 'close',
      onclick: () => {
        onRemove();
      },
    }),
  );
}
