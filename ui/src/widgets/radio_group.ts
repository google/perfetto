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
import {classForIntent, type HTMLAttrs, Intent} from './common';
import {createContext} from '../base/mithril_utils';
import {classNames} from '../base/classnames';
import {Icon} from './icon';

// A group of mutually-exclusive buttons (like radio buttons).
//
// Supports both controlled and uncontrolled usage.
//
// Uncontrolled — the widget tracks its own selection state internally:
//
//   m(RadioGroup, {intent: Intent.Primary}, [
//     m(RadioGroup.Button, {value: 'day'}, 'Day'),
//     m(RadioGroup.Button, {value: 'week'}, 'Week'),
//     m(RadioGroup.Button, {value: 'month'}, 'Month'),
//   ]);
//
// Controlled — the parent owns the selected value and updates it in the
// callback:
//
//   m(RadioGroup, {
//     selectedValue: this.view,
//     onValueChange: (value) => (this.view = value),
//   }, [
//     m(RadioGroup.Button, {value: 'top-down', icon: 'arrow_downward'}, 'Top Down'),
//     m(RadioGroup.Button, {value: 'bottom-up', icon: 'arrow_upward'}, 'Bottom Up'),
//   ]);
export interface RadioGroupAttrs extends HTMLAttrs {
  readonly initialValue?: string;

  // The value of the selected button. Omit for uncontrolled mode.
  readonly selectedValue?: string;

  // Called when the user picks an option.
  readonly onValueChange?: (value: string) => void;

  // Disables all buttons when true.
  // false by default.
  readonly disabled?: boolean;

  // Whether the buttons should stretch to fill the container width.
  // false by default.
  readonly fillWidth?: boolean;

  // What color to make the selected button.
  readonly intent?: Intent;
}

interface RadioGroupState {
  readonly selectedValue?: string;
  readonly onValueChange?: (value: string) => void;
}

const {Consumer, Provider} = createContext<RadioGroupState>({});

export function RadioGroup({
  attrs,
}: m.Vnode<RadioGroupAttrs>): m.Component<RadioGroupAttrs> {
  let selectedValueInternal: string | undefined = attrs.initialValue;

  return {
    view({attrs, children}: m.Vnode<RadioGroupAttrs>) {
      const {
        disabled,
        fillWidth,
        intent = Intent.None,
        selectedValue = selectedValueInternal,
        onValueChange = (value: string) => (selectedValueInternal = value),
        ...htmlAttrs
      } = attrs;
      return m(
        '.pf-radio-group',
        {
          ...htmlAttrs,
          'role': 'radiogroup',
          'disabled': disabled ? true : undefined,
          'aria-disabled': disabled ? 'true' : undefined,
          'className': classNames(
            fillWidth && 'pf-radio-group--fill-width',
            classForIntent(intent),
          ),
        },
        m(Provider, {value: {selectedValue, onValueChange}}, children),
      );
    },
  };
}

export namespace RadioGroup {
  export interface ButtonAttrs extends HTMLAttrs {
    // Unique value for this button, used to identify the selected one.
    readonly value: string;

    // The icon of the button.
    readonly icon?: string;
  }

  export const Button: m.Component<ButtonAttrs> = {
    view({attrs, children}: m.CVnode<ButtonAttrs>) {
      const {value, icon, className, ...htmlAttrs} = attrs;
      return m(Consumer, ({selectedValue, onValueChange}) => {
        const isSelected = selectedValue === value;
        return m(
          '.pf-radio-group__button',
          {
            ...htmlAttrs,
            'role': 'radio',
            'aria-checked': isSelected ? 'true' : 'false',
            'onclick': () => onValueChange?.(value),
            'onkeydown': (e: KeyboardEvent) => {
              if (e.key === ' ' || e.key === 'Enter') {
                e.preventDefault();
                onValueChange?.(value);
              }
            },
            'className': classNames(className, isSelected && 'pf-active'),
          },
          icon && m(Icon, {icon}),
          children,
        );
      });
    },
  };
}
