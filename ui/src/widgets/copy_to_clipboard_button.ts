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
import {ActionButtonHelper} from './action_button_helper';
import {Button, ButtonVariant} from './button';
import {copyToClipboard} from '../base/clipboard';
import {isEmptyVnodes} from '../base/mithril_utils';

export interface CopyToClipboardButtonAttrs {
  readonly className?: string;
  readonly textToCopy: string | (() => string | Promise<string>);
  readonly tooltip?: m.Children;
  readonly title?: string;
  readonly label?: string;
  readonly variant?: ButtonVariant;
  readonly disabled?: boolean;
  readonly compact?: boolean;
}

export function CopyToClipboardButton(): m.Component<CopyToClipboardButtonAttrs> {
  const helper = new ActionButtonHelper();

  return {
    view({attrs}: m.Vnode<CopyToClipboardButtonAttrs>): m.Children {
      const hasLabel = Boolean(attrs.label);
      const label = (function () {
        if (!hasLabel) return '';
        switch (helper.state) {
          case 'idle':
          case 'working':
            return attrs.label;
          case 'done':
            return 'Copied';
        }
      })();

      // Only show default title if no tooltip is provided
      const defaultTitle = isEmptyVnodes(attrs.tooltip)
        ? 'Copy to clipboard'
        : undefined;

      return m(Button, {
        className: attrs.className,
        variant: attrs.variant,
        tooltip: attrs.tooltip,
        title: attrs.title ?? defaultTitle,
        icon: helper.state === 'done' ? Icons.Check : Icons.Copy,
        loading: helper.state === 'working',
        label,
        disabled: attrs.disabled,
        compact: attrs.compact,
        onclick: async () => {
          const textToCopy = attrs.textToCopy;
          await helper.execute(async () => {
            const text =
              typeof textToCopy === 'string'
                ? textToCopy
                : await Promise.resolve(textToCopy());
            await copyToClipboard(text);
          });
        },
      });
    },
  };
}
