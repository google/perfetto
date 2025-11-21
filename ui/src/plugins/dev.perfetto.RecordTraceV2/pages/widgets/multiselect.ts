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
import {ProbeSetting} from '../../config/config_interfaces';
import {MultiSelect, MultiSelectDiff} from '../../../../widgets/multiselect';

export interface CheckboxesAttrs<T> {
  title?: string;
  options: Map<string, T>;
  onChange?: (options: string[]) => void;
  defaultSelected?: string[];
}

export class TypedMultiselect<T> implements ProbeSetting {
  private _selectedKeys: Set<string>;

  constructor(readonly attrs: CheckboxesAttrs<T>) {
    this._selectedKeys = new Set(attrs.defaultSelected ?? []);
  }

  setEnabled(key: string, enabled: boolean) {
    if (enabled) {
      this._selectedKeys.add(key);
    } else {
      this._selectedKeys.delete(key);
    }
  }

  selectedKeys(): string[] {
    return Array.from(this._selectedKeys);
  }

  selectedValues(): T[] {
    const values = [];
    for (const [key, value] of this.attrs.options.entries()) {
      if (this._selectedKeys.has(key)) {
        values.push(value);
      }
    }
    return values;
  }

  serialize() {
    return Array.from(this._selectedKeys);
  }

  deserialize(state: unknown): void {
    if (Array.isArray(state) && state.every((x) => typeof x === 'string')) {
      this._selectedKeys.clear();
      for (const key of state) {
        this.attrs.options.has(key) && this._selectedKeys.add(key);
      }
    }
  }

  render() {
    return [
      this.attrs.title && m('header', this.attrs.title),
      m(MultiSelect, {
        fixedSize: true,
        options: Array.from(this.attrs.options.keys()).map((key) => ({
          id: key,
          name: key,
          checked: this._selectedKeys.has(key),
        })),
        onChange: (diffs: MultiSelectDiff[]) => {
          for (const diff of diffs) {
            this.setEnabled(diff.id, diff.checked);
          }
          this.attrs.onChange?.(Array.from(this._selectedKeys.values()));
        },
      }),
    ];
  }
}
