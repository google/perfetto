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
import {TextInput} from '../../../widgets/text_input';
import {Button} from '../../../widgets/button';
import './alias_tag.scss';

// A tiny tag that expands into a text input when clicked.
// If blurred with an empty value, collapses back to the tag.
export function AliasTag(): m.Component<{
  alias: string;
  placeholder: string;
  onChange: (value: string) => void;
}> {
  let pendingFocus = false;
  let isFocused = false;

  return {
    view({attrs: {alias, placeholder, onChange}}) {
      if (pendingFocus || isFocused || alias !== '') {
        return m(
          '.pf-spag-alias-tag',
          {
            onpointerdown: (e: PointerEvent) => {
              if (!(e.target instanceof HTMLInputElement)) {
                e.preventDefault();
                const input = (e.currentTarget as HTMLElement).querySelector(
                  'input',
                );
                input?.focus();
              }
            },
          },
          [
            m('span', {style: {opacity: 0.5, fontSize: '11px'}}, 'as'),
            m(TextInput, {
              placeholder,
              value: alias,
              autofocus: pendingFocus,
              onChange: (value: string) => onChange(value),
              onfocus: () => {
                isFocused = true;
                pendingFocus = false;
              },
              onblur: (e: FocusEvent) => {
                const value = (e.target as HTMLInputElement).value;
                isFocused = false;
                pendingFocus = false;
                if (value !== alias) {
                  onChange(value);
                }
              },
            }),
          ],
        );
      }
      return m(Button, {
        icon: 'shoppingmode',
        className: 'pf-spag-alias-btn',
        tooltip: 'Add alias',
        onclick: () => {
          pendingFocus = true;
        },
      });
    },
  };
}
