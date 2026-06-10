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
import {assertTrue} from '../../../../base/assert';
import type {ProbeSetting} from '../../config/config_interfaces';
import {Select} from '../../../../widgets/select';

export interface DropdownOption<T extends string | number> {
  readonly value: T;
  readonly label: string;
}

export interface DropdownAttrs<T extends string | number> {
  readonly title: string;
  readonly options: ReadonlyArray<DropdownOption<T>>;
  readonly defaultValue?: T;
}

export class Dropdown<T extends string | number> implements ProbeSetting {
  private _value: T;

  constructor(readonly attrs: DropdownAttrs<T>) {
    assertTrue(attrs.options.length > 0);
    this._value = attrs.defaultValue ?? attrs.options[0].value;
  }

  serialize() {
    return this._value;
  }

  deserialize(state: unknown): void {
    if (typeof state !== 'string' && typeof state !== 'number') {
      return;
    }
    const option = this.attrs.options.find(
      (candidate) => candidate.value === state,
    );
    if (option) {
      this._value = option.value;
    }
  }

  get value(): T {
    return this._value;
  }

  render() {
    return m('.textarea-holder', [
      m('header', this.attrs.title),
      m(
        Select,
        {
          value: String(this._value),
          onchange: (e: Event) => {
            const selectedValue = (e.target as HTMLSelectElement).value;
            const option = this.attrs.options.find(
              (candidate) => String(candidate.value) === selectedValue,
            );
            if (option) {
              this._value = option.value;
            }
          },
        },
        this.attrs.options.map((option) =>
          m('option', {value: String(option.value)}, option.label),
        ),
      ),
    ]);
  }
}
