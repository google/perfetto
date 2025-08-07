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
import {Anchor} from '../../../../widgets/anchor';
import {Icons} from '../../../../base/semantic_icons';

export interface TextareaAttrs {
  placeholder: string;
  title?: string;
  docsLink?: string;
  cssClass?: string;
  default?: string;
  disabled?: boolean;
  onChange?: (text: string) => void;
}

export class Textarea implements ProbeSetting {
  private _text: string;

  constructor(readonly attrs: TextareaAttrs) {
    this._text = this.setText(attrs.default); // re-assignment to make tsc happy.
  }

  setText(text: string | undefined) {
    this._text = text ?? '';
    return this._text;
  }

  get text(): string {
    return this._text;
  }

  serialize() {
    return this._text;
  }

  deserialize(state: unknown): void {
    if (typeof state === 'string') {
      this._text = state;
    }
  }

  render() {
    return m(
      '.textarea-holder',
      m(
        'header',
        this.attrs.title,
        this.attrs.docsLink && [
          ' ',
          m(
            Anchor,
            {icon: Icons.ExternalLink, href: this.attrs.docsLink},
            'Docs',
          ),
        ],
      ),
      m(`textarea.extra-input${this.attrs.cssClass ?? ''}`, {
        onchange: (e: Event) => {
          this.setText((e.target as HTMLTextAreaElement).value);
          this.attrs.onChange?.(this._text);
        },
        disabled: this.attrs.disabled,
        placeholder: this.attrs.placeholder,
        value: this._text,
      }),
    );
  }
}
