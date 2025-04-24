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

type TextInputAttrs = HTMLInputAttrs & {
  // Whether the input should autofocus when it is created.
  readonly autofocus?: boolean;
  // Optional icon to display on the left of the text field.
  readonly leftIcon?: string;
};

export class TextInput implements m.ClassComponent<TextInputAttrs> {
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
    const {leftIcon, className, ...inputAttrs} = attrs; // Destructure icon from other attrs

    return m(
      '.pf-text-input', // Add a wrapper div
      {
        className,
      },
      leftIcon &&
        m(Icon, {icon: leftIcon, className: 'pf-text-input__left-icon'}), // Conditionally render icon
      m('input.pf-text-input__input', {
        ...inputAttrs, // Pass remaining attrs to the input
      }),
    );
  }
}
