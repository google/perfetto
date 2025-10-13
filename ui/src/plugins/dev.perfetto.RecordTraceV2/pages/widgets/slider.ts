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
import {assertTrue} from '../../../../base/logging';
import {exists} from '../../../../base/utils';
import {Icon} from '../../../../widgets/icon';

export interface SliderAttrs {
  title: string;
  values: number[];
  default?: number;
  icon?: string;
  cssClass?: string;
  isTime?: boolean;
  unit: string;
  min?: number;
  description?: string;
  disabled?: boolean;
  zeroIsDefault?: boolean;
  onChange?: (value: number) => void;
}

export class Slider implements ProbeSetting {
  private _value: number;

  constructor(readonly attrs: SliderAttrs) {
    assertTrue(attrs.values.length > 0);
    this._value = this.setValue(undefined);
  }

  serialize() {
    return this._value;
  }

  deserialize(state: unknown): void {
    if (typeof state === 'number') {
      this._value = state;
    }
  }

  get value(): number {
    return this._value;
  }

  setValue(value: number | null | undefined) {
    // Logic if value is null/undefined: try first the .default, if provided,
    // otherwise fall back on the first value of the fixed range... otherwise 0.
    this._value = exists(value)
      ? value
      : this.attrs.default ?? this.attrs.values[0] ?? 0;
    return this._value;
  }

  private onValueChange(newVal: number) {
    this._value = newVal;
    this.attrs.onChange?.(newVal);
  }

  onTimeValueChange(hms: string) {
    try {
      const date = new Date(`1970-01-01T${hms}.000Z`);
      if (isNaN(date.getTime())) return;
      this.onValueChange(date.getTime());
    } catch {}
  }

  onSliderChange(newIdx: number) {
    this.onValueChange(this.attrs.values[newIdx]);
  }

  render() {
    const attrs = this.attrs;
    const id = attrs.title.replace(/[^a-z0-9]/gim, '_').toLowerCase();
    const maxIdx = attrs.values.length - 1;
    const val = this._value;
    let min = attrs.min ?? 1;
    if (attrs.zeroIsDefault) {
      min = Math.min(0, min);
    }
    const description = attrs.description;
    const disabled = attrs.disabled;

    // Find the index of the closest value in the slider.
    let idx = 0;
    for (; idx < attrs.values.length && attrs.values[idx] < val; idx++) {}

    let spinnerCfg = {};
    if (attrs.isTime) {
      const timeStr = new Date(val).toISOString().substring(11, 11 + 8);
      spinnerCfg = {
        type: 'text',
        pattern: '(0[0-9]|1[0-9]|2[0-3])(:[0-5][0-9]){2}', // hh:mm:ss
        defaultValue: timeStr,
        oncreate: (vnode: m.VnodeDOM) => {
          (vnode.dom as HTMLInputElement).value = timeStr;
        },
        onupdate: (vnode: m.VnodeDOM) => {
          const input = vnode.dom as HTMLInputElement;
          // Only update if the input is not focused (i.e., user is not typing)
          if (document.activeElement !== input) {
            input.value = new Date(val).toISOString().substring(11, 11 + 8);
          }
        },
        oninput: (e: InputEvent) => {
          this.onTimeValueChange((e.target as HTMLInputElement).value);
        },
      };
    } else {
      const isDefault = attrs.zeroIsDefault && val === 0;
      spinnerCfg = {
        type: 'number',
        value: isDefault ? '' : val,
        placeholder: isDefault ? '(default)' : '',
        oninput: (e: InputEvent) => {
          this.onValueChange(+(e.target as HTMLInputElement).value);
        },
      };
    }
    return m(
      '.slider' + (attrs.cssClass ?? ''),
      m('header', attrs.title),
      description ? m('header.descr', attrs.description) : '',
      attrs.icon !== undefined && m(Icon, {icon: attrs.icon}),
      m(`input[id="${id}"][type=range][min=0][max=${maxIdx}][value=${idx}]`, {
        disabled,
        oninput: (e: InputEvent) => {
          this.onSliderChange(+(e.target as HTMLInputElement).value);
        },
      }),
      m(`input.spinner[min=${min}][for=${id}]`, spinnerCfg),
      m('.unit', attrs.unit),
    );
  }
}

export const POLL_INTERVAL_SLIDER: SliderAttrs = {
  title: 'Poll interval',
  values: [250, 500, 1000, 2500, 5000, 30000, 60000],
  cssClass: '.thin',
  unit: 'ms',
};
