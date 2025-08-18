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
import {Switch} from '../../../../widgets/switch';

export interface ToggleAttrs {
  title: string;
  descr?: string;
  default?: boolean;
  cssClass?: string;
  onChange?: (enabled: boolean) => void;
}

export class Toggle implements ProbeSetting {
  private _enabled: boolean;

  constructor(readonly attrs: ToggleAttrs) {
    this._enabled = this.setEnabled(undefined);
  }

  setEnabled(enabled: boolean | undefined) {
    this._enabled = enabled ?? this.attrs.default ?? false;
    return this._enabled;
  }

  get enabled(): boolean {
    return this._enabled;
  }

  serialize() {
    return this._enabled;
  }

  deserialize(state: unknown): void {
    if (state === true || state === false) {
      this._enabled = state;
    }
  }

  render() {
    return m('.pf-toggle', {className: this.attrs.cssClass}, [
      m(Switch, {
        className: 'pf-toggle__switch',
        checked: this._enabled,
        oninput: (e: InputEvent) => {
          this.setEnabled((e.target as HTMLInputElement).checked);
          this.attrs.onChange?.(this._enabled);
        },
        label: this.attrs.title,
      }),
      m('.pf-toggle__desc', this.attrs.descr),
    ]);
    return m(
      `.toggle${this._enabled ? '.enabled' : ''}${this.attrs.cssClass ?? ''}`,
      m(
        'label',
        m(`input[type=checkbox]`, {
          checked: this._enabled,
          oninput: (e: InputEvent) => {
            this.setEnabled((e.target as HTMLInputElement).checked);
            this.attrs.onChange?.(this._enabled);
          },
        }),
        m('span', this.attrs.title),
      ),
      m('.descr', this.attrs.descr),
    );
  }
}
