// Copyright (C) 2025 The Android Open Source Project
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
import {Icons} from '../base/semantic_icons';
import {Button, ButtonVariant} from './button';
import {Intent} from './common';

export class CopyHelper {
  private _copied = false;
  private timeoutId: ReturnType<typeof setTimeout> | undefined;
  private readonly timeout: number;

  constructor(timeout = 2000) {
    this.timeout = timeout;
  }

  get copied(): boolean {
    return this._copied;
  }

  async copy(text: string) {
    await navigator.clipboard.writeText(text);
    this._copied = true;
    m.redraw();

    clearTimeout(this.timeoutId);
    this.timeoutId = setTimeout(() => {
      this._copied = false;
      m.redraw();
    }, this.timeout);
  }
}

export interface CopyToClipboardButtonAttrs {
  readonly textToCopy: string;
  readonly title?: string;
  readonly label?: string;
  readonly variant?: ButtonVariant;
}

export function CopyToClipboardButton(): m.Component<CopyToClipboardButtonAttrs> {
  const helper = new CopyHelper();

  return {
    view({attrs}: m.Vnode<CopyToClipboardButtonAttrs>): m.Children {
      const hasLabel = Boolean(attrs.label);
      const label = (function () {
        if (!hasLabel) return '';
        if (helper.copied) return 'Copied';
        return attrs.label;
      })();
      return m(Button, {
        variant: attrs.variant,
        title: attrs.title ?? 'Copy to clipboard',
        icon: helper.copied ? Icons.Check : Icons.Copy,
        intent: helper.copied ? Intent.Success : Intent.None,
        label,
        onclick: async () => {
          await helper.copy(attrs.textToCopy);
        },
      });
    },
  };
}
