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
import './alias_tag.scss';

// A tiny "as" pill that expands into an alias text input when clicked.
// If blurred (or Escape is pressed) with an empty value, collapses back.
export function AliasTag(): m.Component<{
  alias: string;
  placeholder: string;
  onChange: (value: string) => void;
}> {
  let editing = false;

  return {
    view({attrs: {alias, placeholder, onChange}}) {
      if (editing || alias) {
        return m('.pf-spag-alias-tag', [
          m('span.pf-spag-alias-tag__label', 'as'),
          m(TextInput, {
            placeholder,
            value: alias,
            autofocus: editing && !alias,
            onChange: (value: string) => onChange(value),
            onblur: () => {
              if (!alias) editing = false;
            },
            onkeydown: (e: KeyboardEvent) => {
              if (e.key === 'Escape') {
                (e.target as HTMLElement).blur();
              }
            },
          }),
        ]);
      }
      return m(
        'button.pf-spag-alias-btn',
        {
          title: 'Add alias',
          onclick: () => {
            editing = true;
          },
        },
        'as',
      );
    },
  };
}
