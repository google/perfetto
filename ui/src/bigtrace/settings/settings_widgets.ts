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
import {Setting, EnumOption} from './settings_types';
import {Select} from '../../widgets/select';
import {TextInput} from '../../widgets/text_input';
import {
  PopupMultiSelect,
  MultiSelectDiff,
  MultiSelectOption,
} from '../../widgets/multiselect';
import {Checkbox} from '../../widgets/checkbox';
import {Editor} from '../../widgets/editor';

export function renderSetting(setting: Setting<unknown>): m.Children {
  const currentValue = setting.get();
  const disabled = setting.isDisabled();

  switch (setting.type) {
    case 'number':
      return m(TextInput, {
        type: 'number',
        value: String(currentValue),
        placeholder: setting.placeholder,
        disabled,
        onChange: (value: string) => {
          const numValue = parseFloat(value);
          if (!isNaN(numValue)) {
            setting.set(numValue);
          }
        },
      });
    case 'string':
      if (setting.format === 'sql') {
        return m(Editor, {
          text: String(currentValue),
          language: 'perfetto-sql',
          disabled,
          onUpdate: (text: string) => {
            setting.set(text);
          },
        });
      }
      return m(TextInput, {
        value: String(currentValue),
        placeholder: setting.placeholder,
        disabled,
        onChange: (value: string) => {
          setting.set(value);
        },
      });
    case 'boolean':
      return m(Checkbox, {
        checked: Boolean(currentValue),
        disabled,
        onchange: (e: Event) => {
          if (e.currentTarget instanceof HTMLLabelElement) {
            const input = e.currentTarget.querySelector('input');
            if (input) {
              setting.set(input.checked);
            }
          }
        },
      });
    case 'enum':
      const options = setting.options || [];
      return m(
        Select,
        {
          value: String(currentValue),
          disabled,
          onchange: (e: Event) => {
            const target = e.target as HTMLSelectElement;
            setting.set(target.value);
          },
        },
        options.map((option: string | EnumOption) => {
          const value = typeof option === 'string' ? option : option.value;
          const label = typeof option === 'string' ? option : option.label;
          return m(
            'option',
            {
              value: value,
              selected: currentValue === value,
            },
            label,
          );
        }),
      );
    case 'multi-select':
      const multiSelectOptions: MultiSelectOption[] = (
        setting.options || []
      ).map((option) => {
        const value = typeof option === 'string' ? option : option.value;
        const label = typeof option === 'string' ? option : option.label;
        return {
          id: value,
          name: label,
          checked: (currentValue as string[]).includes(value),
        };
      });

      const validSelectedCount = multiSelectOptions.filter(
        (o) => o.checked,
      ).length;

      return m(PopupMultiSelect, {
        label: `${setting.name} (${validSelectedCount} selected)`,
        options: multiSelectOptions,
        onChange: (diffs: MultiSelectDiff[]) => {
          const newValue = [...(currentValue as string[])];
          for (const diff of diffs) {
            if (diff.checked) {
              if (!newValue.includes(diff.id)) {
                newValue.push(diff.id);
              }
            } else {
              const index = newValue.indexOf(diff.id);
              if (index > -1) {
                newValue.splice(index, 1);
              }
            }
          }
          setting.set(newValue);
        },
      });
    case 'string-array':
      return m('textarea.pf-bt-textarea', {
        placeholder: setting.placeholder || 'Comma-separated values',
        disabled,
        rows: 3,
        spellcheck: false,
        oncreate: (vnode: m.VnodeDOM) => {
          (vnode.dom as HTMLTextAreaElement).value = (
            currentValue as string[]
          ).join(', ');
        },
        onblur: (e: Event) => {
          const target = e.target as HTMLTextAreaElement;
          setting.set(
            target.value
              .split(',')
              .map((s) => s.trim())
              .filter((s) => s !== ''),
          );
        },
      });
    default:
      return `Unsupported setting type: ${setting.type}`;
  }
}
