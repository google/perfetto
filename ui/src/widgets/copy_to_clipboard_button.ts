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

export type CopyState = 'idle' | 'working' | 'copied';

export class CopyButtonHelper {
  private _copyState: CopyState = 'idle';
  private copiedTimeoutId: ReturnType<typeof setTimeout> | undefined;
  private loadingTimeoutId: ReturnType<typeof setTimeout> | undefined;
  private readonly timeout: number;
  private readonly loadingDelay: number;

  constructor(timeout = 2000, loadingDelay = 100) {
    this.timeout = timeout;
    this.loadingDelay = loadingDelay;
  }

  get state(): CopyState {
    return this._copyState;
  }

  async copy(textToCopy: string | (() => string | Promise<string>)) {
    clearTimeout(this.copiedTimeoutId);
    clearTimeout(this.loadingTimeoutId);
    this.copiedTimeoutId = undefined;
    this.loadingTimeoutId = undefined;

    // Set to working after a delay
    this.loadingTimeoutId = setTimeout(() => {
      this._copyState = 'working';
      m.redraw();
    }, this.loadingDelay);

    const text =
      typeof textToCopy === 'string'
        ? textToCopy
        : await Promise.resolve(textToCopy());

    await navigator.clipboard.writeText(text);

    // Clear the loading timeout in case copy completed quickly
    clearTimeout(this.loadingTimeoutId);
    this.loadingTimeoutId = undefined;

    this._copyState = 'copied';
    m.redraw();

    this.copiedTimeoutId = setTimeout(() => {
      this._copyState = 'idle';
      m.redraw();
    }, this.timeout);
  }
}

export interface CopyToClipboardButtonAttrs {
  readonly textToCopy: string | (() => string | Promise<string>);
  readonly title?: string;
  readonly label?: string;
  readonly variant?: ButtonVariant;
}

export function CopyToClipboardButton(): m.Component<CopyToClipboardButtonAttrs> {
  const helper = new CopyButtonHelper();

  return {
    view({attrs}: m.Vnode<CopyToClipboardButtonAttrs>): m.Children {
      const hasLabel = Boolean(attrs.label);
      const label = (function () {
        if (!hasLabel) return '';
        switch (helper.state) {
          case 'idle':
          case 'working':
            return attrs.label;
          case 'copied':
            return 'Copied';
        }
      })();
      return m(Button, {
        variant: attrs.variant,
        title: attrs.title ?? 'Copy to clipboard',
        icon: helper.state === 'copied' ? Icons.Check : Icons.Copy,
        loading: helper.state === 'working',
        label,
        onclick: async () => {
          await helper.copy(attrs.textToCopy);
        },
      });
    },
  };
}
