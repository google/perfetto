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
import {classForIntent, HTMLAttrs, Intent} from './common';
import {createContext} from '../base/mithril_utils';
import {classNames} from '../base/classnames';
import {Icon} from './icon';

// A group of mutually-exclusive buttons (like radio buttons).
//
// Supports both controlled and uncontrolled usage.
//
// Uncontrolled — the widget tracks its own selection state internally:
//
//   m(SegmentedButtons, {intent: Intent.Primary}, [
//     m(SegmentedButton, {value: 'day'}, 'Day'),
//     m(SegmentedButton, {value: 'week'}, 'Week'),
//     m(SegmentedButton, {value: 'month'}, 'Month'),
//   ]);
//
// Controlled — the parent owns the selected value and updates it in the
// callback:
//
//   m(SegmentedButtons, {
//     selectedId: this.view,
//     onOptionSelected: (value) => (this.view = value),
//   }, [
//     m(SegmentedButton, {value: 'top-down', icon: 'arrow_downward'}, 'Top Down'),
//     m(SegmentedButton, {value: 'bottom-up', icon: 'arrow_upward'}, 'Bottom Up'),
//   ]);
export interface SegmentedButtonsAttrs extends HTMLAttrs {
  readonly initialValue?: string;

  // The value of the selected button. Omit for uncontrolled mode.
  readonly selectedValue?: string;

  // Called when the user picks a button.
  readonly onOptionSelected?: (value: string) => void;

  // Whether the segmented buttons is disabled.
  // false by default.
  readonly disabled?: boolean;

  // Whether the buttons should stretch to fill the container width.
  // false by default.
  readonly fillWidth?: boolean;

  // What color to make the selected button.
  readonly intent?: Intent;
}

interface SegmentedButtonState {
  readonly selectedValue?: string;
  readonly onchange?: (value: string) => void;
}

const {Consumer, Provider} = createContext<SegmentedButtonState>({});

export function SegmentedButtons({
  attrs,
}: m.Vnode<SegmentedButtonsAttrs>): m.Component<SegmentedButtonsAttrs> {
  let selectedValueInternal: string | undefined = attrs.initialValue;

  return {
    view({attrs, children}: m.Vnode<SegmentedButtonsAttrs>) {
      const {
        disabled,
        fillWidth,
        intent = Intent.None,
        selectedValue = selectedValueInternal,
        onOptionSelected = (value: string) => (selectedValueInternal = value),
        ...htmlAttrs
      } = attrs;
      return m(
        '.pf-segmented-buttons',
        {
          ...htmlAttrs,
          'role': 'radiogroup',
          'aria-disabled': disabled ? 'true' : undefined,
          'className': classNames(
            fillWidth && 'pf-segmented-buttons--fill-width',
            classForIntent(intent),
          ),
        },
        m(
          Provider,
          {value: {selectedValue, onchange: onOptionSelected}},
          children,
        ),
      );
    },
  };
}

export interface SegmentedButtonAttrs extends HTMLAttrs {
  // Unique value for this button, used to identify the selected one.
  readonly value: string;

  // The icon of the button.
  readonly icon?: string;

  // Whether the button is disabled. false by default.
  readonly disabled?: boolean;
}

export const SegmentedButton: m.Component<SegmentedButtonAttrs> = {
  view({attrs, children}: m.CVnode<SegmentedButtonAttrs>) {
    const {value, icon, disabled, className, ...htmlAttrs} = attrs;
    return m(Consumer, ({selectedValue, onchange}) => {
      const isSelected = selectedValue === value;
      return m(
        '.pf-segmented-button',
        {
          ...htmlAttrs,
          'role': 'radio',
          'tabindex': disabled ? -1 : 0,
          'aria-checked': isSelected ? 'true' : 'false',
          'aria-disabled': disabled ? 'true' : undefined,
          'onclick': () => !disabled && onchange?.(value),
          'onkeydown': (e: KeyboardEvent) => {
            if (disabled) return;
            if (e.key === ' ' || e.key === 'Enter') {
              e.preventDefault();
              onchange?.(value);
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
