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
import {Icons} from '../../../base/semantic_icons';
import {Anchor} from '../../../widgets/anchor';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Callout} from '../../../widgets/callout';
import {Intent} from '../../../widgets/common';
import {CopyToClipboardButton} from '../../../widgets/copy_to_clipboard_button';
import {Form, FormLabel} from '../../../widgets/form';
import {Icon} from '../../../widgets/icon';
import {Inline, Stack, StackAuto} from '../../../widgets/stack';
import {LinearProgress} from '../../../widgets/linear_progress';
import {TextInput} from '../../../widgets/text_input';
import {renderErrorDetails} from '../dialogs';
import './share_form.scss';

// Everything the "Share to Google Drive" dialog shows for a trace.
//
// This is owned by the plugin, per trace, rather than by the dialog. The
// dialog can be closed (with Escape or its close button) while an upload is
// still running, and reopening it must show that upload, not a blank form.
export interface ShareState {
  // The name to upload the trace as.
  fileName: string;
  // Where to upload to: a Drive folder, or 'root' for "My Drive".
  location: google.picker.DocumentObject | 'root';
  // The Drive file ID, once the trace is in Drive: either because it was
  // uploaded, or because it was opened from Drive in the first place.
  fileId: string | undefined;
  // The upload in progress, if there is one.
  upload: UploadState | undefined;
  // What went wrong with the last action, if anything. Cleared when the next
  // action starts.
  error: ShareError | undefined;
}

export interface ShareError {
  // What to tell the user.
  readonly message: string;
  // The underlying error, shown under "Error details".
  readonly type: string;
  readonly cause?: unknown;
}

export interface UploadState {
  // Abort this to cancel the upload.
  readonly controller: AbortController;
  // How far the upload has got. Both are 0 until the first progress report
  // arrives, e.g. while connecting to Google.
  uploadedBytes: number;
  totalBytes: number;
}

export interface ShareFormAttrs {
  readonly state: Readonly<ShareState>;
  readonly onFileNameChange: (fileName: string) => void;
  // Starts uploading the trace.
  readonly onUpload: () => void;
  // Cancels the upload in progress.
  readonly onCancelUpload: () => void;
  // Opens the folder picker to change the location.
  readonly onPickFolder: () => void;
  // Opens the Google Drive sharing dialog for the uploaded file.
  readonly onOpenSharingDialog: () => void;
}

// The body of the "Share to Google Drive" dialog. It holds no state of its
// own: it renders the ShareState it is given and reports what the user does
// through the callbacks. It is built from the standard widgets so that it
// looks like the rest of the UI.
export const ShareForm: m.Component<ShareFormAttrs> = {
  view({attrs}) {
    const {fileId} = attrs.state;
    return fileId === undefined || fileId === ''
      ? renderUploadForm(attrs)
      : renderUploaded(attrs, fileId);
  },
};

function renderError(state: Readonly<ShareState>): m.Children {
  if (state.error === undefined) return undefined;
  return m(Callout, {icon: 'error', intent: Intent.Danger}, [
    state.error.message,
    renderErrorDetails(state.error),
  ]);
}

// "Uploading… 42% (12.3 of 29.4 MB)", or just "Uploading…" until the first
// progress report arrives.
function progressText(upload: UploadState): string {
  const {uploadedBytes, totalBytes} = upload;
  if (totalBytes === 0) return 'Uploading…';
  const percent = Math.floor((100 * uploadedBytes) / totalBytes);
  const mb = (bytes: number) => (bytes / 1e6).toFixed(1);
  return `Uploading… ${percent}% (${mb(uploadedBytes)} of ${mb(totalBytes)} MB)`;
}

// The trace is already in Drive: offer ways to get at it.
function renderUploaded(attrs: ShareFormAttrs, fileId: string): m.Children {
  const {fileName} = attrs.state;
  return m(Stack, {className: 'pf-gdrive-share', spacing: 'large'}, [
    renderError(attrs.state),
    m('.pf-gdrive-share__saved', [
      m(Icon, {
        className: 'pf-gdrive-share__saved-icon',
        icon: 'cloud_done',
        filled: true,
      }),
      m('.pf-gdrive-share__saved-text', [
        m('.pf-gdrive-share__saved-title', 'Saved to Google Drive'),
        // The name isn't known when the trace was opened from Drive.
        fileName !== '' && m('.pf-gdrive-share__saved-name', fileName),
      ]),
    ]),
    m(Inline, [
      m(CopyToClipboardButton, {
        label: 'Copy link',
        title: 'Copy a link that opens this trace from Google Drive',
        textToCopy: `${window.location.origin}/?state=${encodeURIComponent(JSON.stringify({ids: [fileId]}))}`,
        variant: ButtonVariant.Filled,
        intent: Intent.Primary,
      }),
      m(Button, {
        label: 'Share',
        icon: 'share',
        title: 'Choose who can open this file in Google Drive',
        variant: ButtonVariant.Filled,
        onclick: () => attrs.onOpenSharingDialog(),
      }),
      m(StackAuto),
      m(
        Anchor,
        {
          href: `https://docs.google.com/file/d/${fileId}/view`,
          target: '_blank',
          rel: 'noopener',
          icon: Icons.ExternalLink,
        },
        'Open in Drive',
      ),
    ]),
  ]);
}

// The trace is not in Drive yet: ask for a name and a folder, then upload.
function renderUploadForm(attrs: ShareFormAttrs): m.Children {
  const {state} = attrs;
  const {location, upload} = state;
  const uploading = upload !== undefined;
  return m(
    Form,
    {
      className: 'pf-gdrive-share',
      // Form disables the submit button while this returns false, as well as
      // while a required field is empty.
      validation: () => !uploading,
      onsubmit: (e: Event) => {
        e.preventDefault();
        attrs.onUpload();
      },
    },
    [
      renderError(state),
      m(
        'p.pf-gdrive-share__intro',
        'Upload this trace to your Google Drive to get a link you can share.',
      ),
      m(FormLabel, {for: 'pf-gdrive-share-filename'}, 'File name'),
      m(TextInput, {
        id: 'pf-gdrive-share-filename',
        value: state.fileName,
        placeholder: 'my-trace.perfetto-trace',
        required: true,
        autofocus: true,
        // The name and location cannot change once an upload has started.
        disabled: uploading,
        onInput: (value: string) => attrs.onFileNameChange(value),
      }),
      m(FormLabel, {for: 'pf-gdrive-share-location'}, 'Location'),
      // The location can only be chosen with the folder picker, so it's shown
      // as a single button that opens the picker, rather than a read-only
      // text box that looks editable.
      m(Button, {
        id: 'pf-gdrive-share-location',
        className: 'pf-gdrive-share__location',
        type: 'button',
        icon: 'folder',
        label: location === 'root' ? 'My Drive' : (location.name ?? 'Folder'),
        rightIcon: 'edit',
        title: 'Choose a folder in Google Drive',
        variant: ButtonVariant.Outlined,
        disabled: uploading,
        onclick: () => attrs.onPickFolder(),
      }),
      upload === undefined
        ? m(Inline, {className: 'pf-gdrive-share__footer'}, [
            m(StackAuto),
            m(Button, {
              type: 'submit',
              label: 'Upload',
              icon: 'upload',
              intent: Intent.Primary,
              variant: ButtonVariant.Filled,
            }),
          ])
        : m('.pf-gdrive-share__progress', [
            m(LinearProgress, {
              className: 'pf-gdrive-share__progress-bar',
              // Animate until the first progress report says how big the
              // upload is.
              state: 'indeterminate',
              value:
                upload.totalBytes > 0
                  ? upload.uploadedBytes / upload.totalBytes
                  : undefined,
            }),
            m(Inline, [
              m('span.pf-gdrive-share__progress-text', progressText(upload)),
              m(StackAuto),
              m(Button, {
                type: 'button',
                label: 'Cancel',
                variant: ButtonVariant.Outlined,
                onclick: () => attrs.onCancelUpload(),
              }),
            ]),
          ]),
    ],
  );
}
