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
import {HTMLInputAttrs} from './common';
import {Icon} from './icon'; // Import Icon component

export type TextInputAttrs = HTMLInputAttrs & {
  // Whether the input should autofocus when it is created.
  readonly autofocus?: boolean;
  // Optional icon to display on the left of the text field.
  readonly leftIcon?: string;
  // Callback fired when the input value changes (on every keystroke).
  readonly onInput?: (value: string) => void;
  // Callback fired when the input loses focus or enter is pressed.
  readonly onChange?: (value: string) => void;
};

export class TextInput implements m.ClassComponent<TextInputAttrs> {
  // When onChange is used without onInput, we need local state to prevent
  // Mithril redraws from resetting the input value mid-edit.
  private editing = false;
  private localValue = '';

  oncreate(vnode: m.CVnodeDOM<TextInputAttrs>) {
    if (vnode.attrs.autofocus) {
      // Focus the actual input element inside the wrapper
      const inputElement = vnode.dom.querySelector('input');
      if (inputElement) {
        inputElement.focus();
      }
    }
  }

  view({attrs}: m.CVnode<TextInputAttrs>) {
    const {leftIcon, className, onInput, onChange, ...inputAttrs} = attrs;

    // When using onChange without onInput, manage local state to avoid
    // Mithril resetting the input during typing.
    const useDeferredMode = onChange && !onInput;
    const displayValue =
      useDeferredMode && this.editing ? this.localValue : inputAttrs.value;

    return m(
      '.pf-text-input',
      {
        className,
      },
      leftIcon &&
        m(Icon, {icon: leftIcon, className: 'pf-text-input__left-icon'}),
      m('input.pf-text-input__input', {
        ...inputAttrs,
        value: displayValue,
        oninput: useDeferredMode
          ? (e: InputEvent) => {
              inputAttrs.oninput?.(e);
              this.editing = true;
              this.localValue = (e.target as HTMLInputElement).value;
            }
          : onInput
            ? (e: InputEvent) => {
                inputAttrs.oninput?.(e);
                onInput((e.target as HTMLInputElement).value);
              }
            : inputAttrs.oninput,
        onchange: onChange
          ? (e: InputEvent) => {
              inputAttrs.onchange?.(e);
              this.editing = false;
              onChange((e.target as HTMLInputElement).value);
            }
          : inputAttrs.onchange,
      }),
    );
  }
}
