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

import {classNames} from '../base/classnames';

export interface SwitchAttrs {
  // Optional text to show to the right of the switch.
  // If omitted, no text will be shown.
  label?: string;
  // Whether the switch is checked or not.
  // If omitted, the switch will be uncontrolled.
  checked?: boolean;
  // Make the switch appear greyed out block any interaction with it.
  // No events will be fired when interacting with it.
  // Defaults to false.
  disabled?: boolean;
  // Remaining attributes forwarded to the underlying HTML <label>.
  [htmlAttrs: string]: any;
}

export class Switch implements m.ClassComponent<SwitchAttrs> {
  view({attrs}: m.CVnode<SwitchAttrs>) {
    const {label, checked, disabled = false, ...htmlAttrs} = attrs;

    const classes = classNames(
        disabled && 'pf-disabled',
    );

    // The default checkbox is removed and an entirely new one created inside
    // the span element in CSS.
    return m(
        'label.pf-switch',
        {class: classes, ...htmlAttrs},
        m('input[type=checkbox]', {disabled, checked}),
        m('span'),
        label,
    );
  }
}
