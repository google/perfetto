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

// Material Design outlined input/select field with label on border.
// Pass children as the third argument to m() for select options.
export interface OutlinedFieldAttrs {
  label: string;
  value: string;
  onchange?: (e: Event) => void;
  oninput?: (e: Event) => void;
  disabled?: boolean;
  placeholder?: string; // For text inputs
}

export class OutlinedField implements m.ClassComponent<OutlinedFieldAttrs> {
  view({attrs, children}: m.Vnode<OutlinedFieldAttrs>) {
    const {label, value, onchange, oninput, disabled, placeholder} = attrs;

    // Determine if this is a select or input
    // Children can be an array, so check if it has content
    const isSelect =
      children !== undefined &&
      children !== null &&
      (Array.isArray(children) ? children.length > 0 : true);

    return m(
      'fieldset.pf-outlined-field',
      {
        disabled,
      },
      [
        m('legend.pf-outlined-field-legend', label),
        isSelect
          ? m(
              'select.pf-outlined-field-input',
              {
                value,
                onchange,
                disabled,
              },
              children,
            )
          : m('input.pf-outlined-field-input', {
              type: 'text',
              value,
              oninput,
              disabled,
              placeholder,
            }),
      ],
    );
  }
}

// Read-only version of OutlinedField for displaying static information.
// Uses the same visual style but shows text instead of an input.
export interface OutlinedFieldReadOnlyAttrs {
  label: string;
  value: string;
  className?: string;
}

export class OutlinedFieldReadOnly
  implements m.ClassComponent<OutlinedFieldReadOnlyAttrs>
{
  view({attrs}: m.Vnode<OutlinedFieldReadOnlyAttrs>) {
    const {label, value, className} = attrs;

    return m(
      'fieldset.pf-outlined-field',
      {className},
      m('legend.pf-outlined-field-legend', label),
      m('.pf-outlined-field-input.pf-read-only', value),
    );
  }
}
