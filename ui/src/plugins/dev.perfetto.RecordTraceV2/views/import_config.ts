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
import {Button, ButtonBar, ButtonVariant} from '../../../widgets/button';
import {errResult, okResult, Result} from '../../../base/result';

export interface ImportConfigDialogAttrs {
  readonly config: string;
  readonly onUpdate: (config: string) => void;
}

export function ImportConfigDialog(): m.Component<ImportConfigDialogAttrs> {
  return {
    view({attrs}: m.Vnode<ImportConfigDialogAttrs>) {
      const {config, onUpdate} = attrs;
      return m('.pf-import-config', [
        m(ButtonBar, [
          m(Button, {
            label: 'Import from file',
            icon: 'file_upload',
            variant: ButtonVariant.Filled,
            onclick: async () => {
              const file = await importFile();
              if (file.ok) {
                const text = await file.value.text();
                m.redraw();
                onUpdate(text);
              }
            },
          }),
        ]),
        m('textarea.pf-import-config__textarea', {
          value: config,
          oninput: (e: Event) =>
            onUpdate((e.target as HTMLTextAreaElement).value),
        }),
      ]);
    },
  };
}

function importFile(): Promise<Result<File>> {
  return new Promise((resolve) => {
    const input = document.createElement('input');
    input.type = 'file';
    input.onchange = async () => {
      m.redraw();
      const file = input.files?.[0];
      if (file) {
        resolve(okResult(file));
      } else {
        resolve(errResult('No file selected'));
      }
    };
    input.click();
  });
}
