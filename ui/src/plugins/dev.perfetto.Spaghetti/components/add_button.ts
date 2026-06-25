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
import {Button} from '../../../widgets/button';
import './add_button.scss';

export interface AddButtonAttrs {
  readonly label: string;
  readonly onclick: () => void;
}

// Subtle full-width button for appending a row to a list.
export const AddButton: m.Component<AddButtonAttrs> = {
  view({attrs}) {
    return m(Button, {
      label: attrs.label,
      icon: 'add',
      className: 'pf-spag-add-btn',
      onclick: attrs.onclick,
    });
  },
};

export interface InputCountButtonsAttrs {
  // Hide the remove button when the input count is at its minimum.
  readonly canRemove: boolean;
  readonly onAdd: () => void;
  readonly onRemove: () => void;
}

// Side-by-side "− Input" / "+ Input" buttons for nodes with a variable
// number of input ports, styled like AddButton.
export const InputCountButtons: m.Component<InputCountButtonsAttrs> = {
  view({attrs}) {
    return m('.pf-spag-input-count', [
      attrs.canRemove &&
        m(Button, {
          label: 'Input',
          icon: 'remove',
          className: 'pf-spag-add-btn',
          onclick: attrs.onRemove,
        }),
      m(Button, {
        label: 'Input',
        icon: 'add',
        className: 'pf-spag-add-btn',
        onclick: attrs.onAdd,
      }),
    ]);
  },
};
