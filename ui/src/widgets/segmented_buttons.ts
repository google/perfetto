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
import {HTMLAttrs} from './common';
import {classNames} from '../base/classnames';

interface Option {
  // Label buttons require a label.
  readonly label: string;
  // Label buttons can have an optional icon.
  readonly icon?: string;
}

export interface SegmentedButtonsAttrs extends HTMLAttrs {
  // Options for segmented buttons.
  readonly options: ReadonlyArray<Option>;

  // The index of the selected button.
  readonly selectedOption: number;

  // Callback function which is called every time a
  readonly onOptionSelected: (num: number) => void;

  // Whether the segmented buttons is disabled.
  // false by default.
  readonly disabled?: boolean;
}

export class SegmentedButtons
  implements m.ClassComponent<SegmentedButtonsAttrs>
{
  view({attrs}: m.CVnode<SegmentedButtonsAttrs>) {
    const {options, selectedOption, onOptionSelected, ...htmlAttrs} = attrs;
    return m(
      '.pf-segmented-buttons',
      htmlAttrs,
      options.map((o, i) => {
        const className = classNames(
          i === selectedOption && 'pf-segmented-buttons__button--active',
        );
        return m(
          '.pf-segmented-buttons__button',
          {
            onclick: () => onOptionSelected(i),
            className,
          },
          o.label,
        );
      }),
    );
  }
}
