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
import {download, FilePickerOptions} from '../base/download_utils';
import {Icons} from '../base/semantic_icons';
import {ActionButtonHelper} from './action_button_helper';
import {Button, ButtonVariant} from './button';

export interface DownloadToFileButtonAttrs {
  readonly content:
    | string
    | Blob
    | Uint8Array
    | (() => string | Blob | Uint8Array | Promise<string | Blob | Uint8Array>);
  readonly fileName: string;
  readonly title?: string;
  readonly label?: string;
  readonly variant?: ButtonVariant;
  readonly filePicker?: FilePickerOptions;
}

export class DownloadToFileButton
  implements m.ClassComponent<DownloadToFileButtonAttrs>
{
  private helper = new ActionButtonHelper();

  view({attrs}: m.Vnode<DownloadToFileButtonAttrs>): m.Children {
    const hasLabel = Boolean(attrs.label);
    const label = (function (state) {
      if (!hasLabel) return '';
      switch (state) {
        case 'idle':
        case 'working':
          return attrs.label;
        case 'completed':
          return 'Downloaded';
      }
    })(this.helper.state);

    return m(Button, {
      variant: attrs.variant,
      title: attrs.title ?? 'Download to file',
      icon: this.helper.state === 'completed' ? Icons.Check : Icons.Download,
      loading: this.helper.state === 'working',
      label,
      onclick: async () => {
        await this.helper.execute(async () => {
          const content =
            typeof attrs.content === 'function'
              ? await Promise.resolve(attrs.content())
              : attrs.content;
          await download({
            content,
            fileName: attrs.fileName,
            filePicker: attrs.filePicker,
          });
        });
      },
    });
  }
}
