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
import {AppImpl} from '../../core/app_impl';
import {TraceImpl} from '../../core/trace_impl';
import {PerfettoPlugin} from '../../public/plugin';
import {Anchor} from '../../widgets/anchor';
import {Button, ButtonVariant} from '../../widgets/button';
import {closeModal, showModal} from '../../widgets/modal';
import {Stack, StackAuto} from '../../widgets/stack';
import {TextInput} from '../../widgets/text_input';
import {GoogleDriveClient, GoogleDriveFile} from './gdrive_client';
import {Result} from '../../base/result';
import {Intent} from '../../widgets/common';
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';

// Note: keys purposely stripped for now...
const API_KEY = '';
const CLIENT_ID = '';
const APP_ID = '';
const gDriveClient = new GoogleDriveClient(API_KEY, CLIENT_ID, APP_ID);
const uploadedTraces = new WeakMap<TraceImpl, string>();
let pendingTraceFileId: string | undefined = undefined;

// TODO(stevegolton): Add the ability for the app to be able to open a google
// drive file by clicking through the open with menu of the google drive UI.
// This will require the app to be registered with google and the correct
// redirect URIs to be set up - i.e. https://perfetto.dev/?state={...} <-- JSON
// blob represents a file

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.GoogleDrive';
  static readonly description = 'Open and save traces to Google Drive';

  static onActivate(app: AppImpl) {
    app.sidebar.addMenuItem({
      section: 'navigation',
      text: 'Open from Google Drive',
      icon: 'add_to_drive',
      action: async () => {
        const auth = await gDriveClient.authenticate();
        if (auth.response !== 'success') return;
        const files = await gDriveClient.pickFile(auth.accessToken);
        if (!files) return;
        if (files.length === 0) return;

        const firstFile = files[0];
        const fileResult = await gDriveClient.openFile(
          auth.accessToken,
          firstFile.id,
        );
        if (fileResult.ok) {
          const file = fileResult.value;
          pendingTraceFileId = file.id;
          app.openTraceFromBuffer({
            buffer: await file.blob.arrayBuffer(),
            title: file.name || 'Google Drive Trace',
            fileName: file.name || 'gdrive-trace.pftrace',
          });
        }
      },
    });

    const args = app.initialPluginRouteArgs;
    const fileId = args['openFileId'];

    if (fileId !== undefined && typeof fileId === 'string') {
      handlePermalink(app, fileId);
    }
  }

  async onTraceLoad(trace: TraceImpl): Promise<void> {
    if (pendingTraceFileId) {
      uploadedTraces.set(trace, pendingTraceFileId);
      pendingTraceFileId = undefined;
    }

    trace.sidebar.addMenuItem({
      section: 'current_trace',
      text: 'Share to Google Drive',
      icon: 'add_to_drive',
      sortOrder: 10,
      action: () => {
        // This button opens a modal dialog which displays the state of the file
        // in google drive allowing the user to upload it and optionally share
        // it. The dialog gets the state of whether the file is already in
        // google drive from the trace type + the fileId.
        showModal({
          key: 'GDriveUpload',
          title: 'Google Drive',
          content: () => m(UploadTraceModal, {trace, gDriveClient}),
        });
      },
    });
  }
}

async function openGoogleDriveTrace(
  app: AppImpl,
  token: string,
  fileId: string,
): Promise<Result<GoogleDriveFile>> {
  const fileResult = await gDriveClient.openFile(token, fileId);
  if (fileResult.ok) {
    const file = fileResult.value;
    app.openTraceFromBuffer({
      buffer: await file.blob.arrayBuffer(),
      title: file.name,
      // fileName: file.name,
    });
    pendingTraceFileId = file.id;
  }
  return fileResult;
}

async function handlePermalink(app: AppImpl, fileId: string) {
  // Here we should check to see if we actually need to authenticate or wether
  // we can get away with using our cached token. If we do need to authenticate,
  // use full screen authentication as the normal authentication would probably
  // be blocked by the popup blocker in the browser. We might need to
  // authenticate first, use fullscreen authentication.

  const auth = await gDriveClient.authenticate();
  if (auth.response === 'popup_blocked') {
    console.log('Popup blocked, need to show a dialog to the user.');
    showModal({
      key: 'GoogleDrivePopupBlocked',
      title: 'Popups blocked',
      content: () =>
        m(Stack, [
          m(
            'p',
            'Google drive authentication requires a popup, please disable popups on this website and reload the page.',
          ),
          m(Stack, {orientation: 'horizontal'}, [
            m(Button, {
              label: 'Try again',
              intent: Intent.Primary,
              variant: ButtonVariant.Filled,
              onclick: async () => {
                window.location.reload();
              },
            }),
            m(Button, {
              label: 'Reload',
              intent: Intent.Primary,
              variant: ButtonVariant.Filled,
              onclick: async () => {
                window.location.reload();
              },
            }),
          ]),
        ]),
    });
  }

  if (auth.response !== 'success') {
    console.log('Failed to authenticate with Google Drive');
    return;
  }

  const accessToken = auth.accessToken;
  const fileResult = await openGoogleDriveTrace(app, accessToken, fileId);
  if (!fileResult.ok) {
    // Maybe we need to authorize access to the file from our app?
    showModal({
      key: 'GoogleDriveAuthNeeded',
      title: 'Authorize access to file',
      content: () =>
        m(Stack, [
          m(
            'p',
            "This is the first time you've accessed this app through the drive, please authorize access to the file.",
          ),
          m(Button, {
            label: 'Authorize File Access',
            intent: Intent.Primary,
            variant: ButtonVariant.Filled,
            onclick: async () => {
              const files = await gDriveClient.requestFileAccess(
                accessToken,
                fileId,
              );
              if (!files) return;
              await openGoogleDriveTrace(app, accessToken, fileId);
              closeModal('GoogleDriveAuthNeeded');
              // TODO(stevegolton): If this didn't work, I'm out of ideas!
            },
          }),
        ]),
    });
    console.log(fileResult);
  }
}

interface UploadTraceModalAttrs {
  readonly trace: TraceImpl;
  readonly gDriveClient: GoogleDriveClient;
}

function UploadTraceModal(): m.Component<UploadTraceModalAttrs> {
  let fileName = '';
  let location: 'root' | google.picker.DocumentObject = 'root';
  let uploading = false;

  return {
    oninit({attrs}) {
      fileName = attrs.trace.traceInfo.traceTitle ?? '';
    },
    view({attrs}) {
      const {trace} = attrs;
      const fileId = uploadedTraces.get(trace);

      if (fileId) {
        // The file is already uploaded.
        return m(Stack, {spacing: 'large'}, [
          m(
            Anchor,
            {
              href: `https://docs.google.com/file/d/${fileId}/view`,
              target: '_blank',
            },
            'View file in Google Drive',
          ),
          m(Stack, {orientation: 'horizontal'}, [
            m(CopyToClipboardButton, {
              textToCopy: `${window.location.origin}#!/?dev.perfetto.GoogleDrive:openFileId=${fileId}`,
              title: 'Copy link',
              label: 'Copy link',
              intent: Intent.Primary,
              variant: ButtonVariant.Filled,
            }),
            m(Button, {
              label: 'Change ACLs',
              onclick: async () => {
                const auth = await attrs.gDriveClient.authenticate();
                if (auth.response === 'success') {
                  attrs.gDriveClient.openSharingDialog(
                    auth.accessToken,
                    fileId,
                  );
                }
              },
            }),
          ]),
        ]);
      } else {
        // The file is not yet uploaded.
        return m(Stack, {spacing: 'large'}, [
          m(Stack, {spacing: 'medium'}, [
            m(Stack, {orientation: 'horizontal'}, [
              'Filename: ',
              m(TextInput, {
                className: 'pf-gdrive-share__filename',
                value: fileName,
                oninput: (e: Event) => {
                  fileName = (e.target as HTMLInputElement).value;
                },
                placeholder: 'Enter filename...',
              }),
            ]),
            m(Stack, {orientation: 'horizontal'}, [
              'Location: ',
              location === 'root' ? 'My Drive' : location.name,
              m(StackAuto),
              m(Button, {
                label: 'Change location...',
                onclick: async () => {
                  const auth = await attrs.gDriveClient.authenticate();
                  if (auth.response !== 'success') return;
                  const folder = await attrs.gDriveClient.pickFolder(
                    auth.accessToken,
                  );
                  if (!folder) return;
                  location = folder;
                  m.redraw();
                },
              }),
            ]),
          ]),
          m(Button, {
            loading: uploading,
            disabled: fileName.length === 0 || uploading,
            onclick: async () => {
              uploading = true;
              const auth = await attrs.gDriveClient.authenticate();
              if (auth.response !== 'success') return;
              const traceBlob = await trace.getTraceFile();
              const result = await attrs.gDriveClient.uploadFile(
                auth.accessToken,
                traceBlob,
                location === 'root' ? 'root' : location.id,
                fileName,
              );
              if (result.ok) {
                uploadedTraces.set(trace, result.value);
              }

              m.redraw();
            },
            intent: Intent.Primary,
            variant: ButtonVariant.Filled,
            label: 'Upload',
          }),
        ]);
      }
    },
  };
}
