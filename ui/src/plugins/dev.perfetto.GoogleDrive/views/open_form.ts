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
import {Button, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {Form, FormLabel} from '../../../widgets/form';
import {Inline, Stack} from '../../../widgets/stack';
import {TextInput} from '../../../widgets/text_input';
import {
  type GDriveOpenFileRequest,
  parseGDriveLink,
} from '../google/drive_client';
import './open_form.scss';

export interface OpenFormAttrs {
  readonly link: string;
  readonly onLinkChange: (link: string) => void;
  readonly onBrowse: () => void;
  readonly onOpenLink: (file: GDriveOpenFileRequest) => void;
}

// Body of the "Open from Google Drive" dialog, offering either the Drive file
// picker or pasting a Google Drive file link.
export const OpenForm: m.Component<OpenFormAttrs> = {
  view({attrs}) {
    const trimmed = attrs.link.trim();
    const parsed = parseGDriveLink(attrs.link);

    return m(Stack, {className: 'pf-gdrive-open', spacing: 'large'}, [
      m(Button, {
        label: 'Browse Google Drive',
        icon: 'folder_open',
        variant: ButtonVariant.Filled,
        intent: Intent.Primary,
        onclick: () => attrs.onBrowse(),
      }),
      m('.pf-gdrive-open__divider', 'or'),
      m(
        Form,
        {
          validation: () => parsed.ok,
          onsubmit: (e: Event) => {
            e.preventDefault();
            if (parsed.ok) {
              attrs.onOpenLink(parsed.value);
            }
          },
        },
        [
          m(
            FormLabel,
            {for: 'pf-gdrive-open-link'},
            'Paste a Google Drive link',
          ),
          m(Inline, [
            m(TextInput, {
              id: 'pf-gdrive-open-link',
              className: 'pf-gdrive-open__link-input',
              value: attrs.link,
              placeholder: 'https://drive.google.com/file/d/...',
              onInput: (value: string) => attrs.onLinkChange(value),
            }),
            m(Button, {
              type: 'submit',
              label: 'Open',
              variant: ButtonVariant.Filled,
              disabled: !parsed.ok,
            }),
          ]),
          trimmed !== '' &&
            !parsed.ok &&
            m('.pf-gdrive-open__error', parsed.error),
        ],
      ),
    ]);
  },
};
