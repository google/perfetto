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
import {Icon} from '../../widgets/icon';

export interface ShareFormAttrs {
  // The initial filename to prefill in the input.
  readonly initialFileName: string;
  // The Google Drive file ID if the trace has already been uploaded.
  readonly fileId?: string;
  // Uploads the trace to the given location ('root' or a folder ID).
  readonly onUpload: (fileName: string, location: string) => Promise<void>;
  // Opens the folder picker; returns the selected folder or undefined if
  // cancelled.
  readonly onPickFolder: () => Promise<void>;
  // Opens the Google Drive sharing dialog for the uploaded file.F
  readonly onOpenSharingDialog: () => Promise<void>;
  // The currently selected upload location - a document object or 'root' for
  // simply the root / 'My drive'.
  readonly location: google.picker.DocumentObject | 'root';
}

export function ShareForm(): m.Component<ShareFormAttrs> {
  let fileName = '';
  let uploading = false;

  return {
    oninit({attrs}) {
      fileName = attrs.initialFileName;
    },
    view({attrs}) {
      const {fileId, onUpload, onPickFolder, onOpenSharingDialog, location} =
        attrs;

      if (fileId) {
        // Already uploaded state.
        return m('.pf-gdrive-share', [
          m('.pf-gdrive-share__success', [
            m('.pf-gdrive-share__success-icon', [
              m(Icon, {icon: 'check_circle', filled: true}),
            ]),
            m('.pf-gdrive-share__success-text', [
              m('h3.pf-gdrive-share__success-title', 'Trace uploaded'),
              m(
                'p.pf-gdrive-share__success-desc',
                'Your trace has been saved to Google Drive.',
              ),
            ]),
          ]),
          m('.pf-gdrive-share__actions', [
            m(
              'a',
              {
                class: 'pf-gdrive-share__btn pf-gdrive-share__btn--primary',
                href: `https://docs.google.com/file/d/${fileId}/view`,
                target: '_blank',
                rel: 'noopener',
              },
              [
                m(Icon, {
                  icon: 'folder_open',
                  class: 'pf-gdrive-share__btn-icon',
                }),
                m('span', 'Open in Drive'),
              ],
            ),
            m(
              'button',
              {
                class: 'pf-gdrive-share__btn pf-gdrive-share__btn--secondary',
                onclick: async () => {
                  const url = `${window.location.origin}#!/?dev.perfetto.GoogleDrive:openFileId=${fileId}`;
                  await navigator.clipboard.writeText(url);
                },
              },
              [
                m(Icon, {icon: 'link', class: 'pf-gdrive-share__btn-icon'}),
                m('span', 'Copy link'),
              ],
            ),
            m(
              'button',
              {
                class: 'pf-gdrive-share__btn pf-gdrive-share__btn--ghost',
                onclick: () => onOpenSharingDialog(),
              },
              [
                m(Icon, {icon: 'share', class: 'pf-gdrive-share__btn-icon'}),
                m('span', 'Share'),
              ],
            ),
          ]),
        ]);
      }

      // Upload form state.
      return m('.pf-gdrive-share', [
        m('.pf-gdrive-share__form', [
          // Filename field
          m('.pf-gdrive-share__field', [
            m(
              'label',
              {class: 'pf-gdrive-share__label', for: 'gdrive-filename'},
              ['Filename'],
            ),
            m('input', {
              id: 'gdrive-filename',
              class: 'pf-gdrive-share__input',
              type: 'text',
              value: fileName,
              placeholder: 'my-trace.perfetto-trace',
              oninput: (e: Event) => {
                fileName = (e.target as HTMLInputElement).value;
              },
              onkeydown: (e: KeyboardEvent) => {
                if (e.key === 'Enter' && fileName && !uploading) {
                  void handleUpload();
                }
              },
            }),
          ]),

          // Location field
          m('.pf-gdrive-share__field', [
            m('label.pf-gdrive-share__label', 'Location'),
            m('.pf-gdrive-share__location', [
              m('.pf-gdrive-share__location-value', [
                m(Icon, {
                  icon: 'folder',
                  class: 'pf-gdrive-share__location-icon',
                }),
                m('span', location === 'root' ? 'My Drive' : location.name),
              ]),
              m(
                'button',
                {
                  class: 'pf-gdrive-share__location-change',
                  onclick: async () => {
                    await onPickFolder();
                    m.redraw();
                  },
                },
                'Change',
              ),
            ]),
          ]),
        ]),

        // Upload button
        m('.pf-gdrive-share__footer', [
          m(
            'button',
            {
              class: `pf-gdrive-share__upload-btn${uploading ? ' pf-gdrive-share__upload-btn--loading' : ''}`,
              disabled: !fileName || uploading,
              onclick: () => void handleUpload(),
            },
            [
              uploading
                ? m('span.pf-gdrive-share__spinner')
                : m(Icon, {
                    icon: 'upload',
                    class: 'pf-gdrive-share__upload-btn-icon',
                  }),
              m('span', uploading ? 'Uploading…' : 'Upload'),
            ],
          ),
        ]),
      ]);

      async function handleUpload() {
        if (!fileName || uploading) return;
        uploading = true;
        m.redraw();
        try {
          await onUpload(fileName, location === 'root' ? 'root' : location.id);
        } finally {
          uploading = false;
          m.redraw();
        }
      }
    },
  };
}
