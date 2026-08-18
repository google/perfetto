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
import type {AppImpl} from '../../core/app_impl';
import type {TraceImpl} from '../../core/trace_impl';
import type {PerfettoPlugin} from '../../public/plugin';
import type {RouteArgs} from '../../public/route_schema';
import {Anchor} from '../../widgets/anchor';
import {Button, ButtonVariant} from '../../widgets/button';
import {closeModal, showModal} from '../../widgets/modal';
import {Stack, StackAuto} from '../../widgets/stack';
import {TextInput} from '../../widgets/text_input';
import {GoogleDriveClient, type GoogleDriveFile} from './gdrive_client';
import type {Result} from '../../base/result';
import {defer} from '../../base/deferred';
import {Intent} from '../../widgets/common';
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';
import './styles.scss';

// The Google Drive credentials are not checked into the repo. They are loaded
// at runtime from an internal script (the same way the internal userscript is
// loaded) via a <script> tag, which sidesteps the CORS restrictions a
// cross-origin fetch() would hit. The script populates window.gDriveConfig.
const CONFIG_URL =
  'https://storage.cloud.google.com/perfetto-ui-internal/gdrive_config.js';
const CONFIG_LOAD_TIMEOUT_MS = 5000;

interface GDriveConfig {
  readonly apiKey: string;
  readonly clientId: string;
  readonly appId: string;
}

declare global {
  interface Window {
    gDriveConfig?: GDriveConfig;
  }
}

// Kicks off the config script load and returns a promise that resolves to the
// populated window.gDriveConfig (or empty defaults on error/timeout).
function loadGDriveConfig(): Promise<GDriveConfig> {
  const defaults: GDriveConfig = {apiKey: '', clientId: '', appId: ''};
  window.gDriveConfig = defaults;

  const loaded = defer<GDriveConfig>();
  const script = document.createElement('script');
  script.src = CONFIG_URL;
  script.async = true;
  script.onload = () => loaded.resolve(window.gDriveConfig ?? defaults);
  script.onerror = () => loaded.resolve(defaults);
  document.head.append(script);

  setTimeout(
    () => loaded.resolve(window.gDriveConfig ?? defaults),
    CONFIG_LOAD_TIMEOUT_MS,
  );
  return loaded;
}

let gDriveClientPromise: Promise<GoogleDriveClient> | undefined;

// Lazily loads the config and constructs the client on first use, caching the
// result so the config is only loaded once.
function getGDriveClient(): Promise<GoogleDriveClient> {
  if (!gDriveClientPromise) {
    gDriveClientPromise = loadGDriveConfig().then(
      (config) =>
        new GoogleDriveClient(config.apiKey, config.clientId, config.appId),
    );
  }
  return gDriveClientPromise;
}

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

  static onActivate(app: AppImpl, pluginArgs: RouteArgs) {
    app.sidebar.addMenuItem({
      section: 'trace_files',
      text: 'Open from GDrive',
      icon: 'drive_export',
      badge: 'preview',
      visible: () => app.isInternalUser,
      action: async () => {
        const client = await getGDriveClient();
        const auth = await client.authenticate();
        if (auth.response !== 'success') return;
        const files = await client.pickFile(auth.accessToken);
        if (!files) return;
        if (files.length === 0) return;

        const firstFile = files[0];
        const fileResult = await client.openFile(
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

    const fileId = pluginArgs['openFileId'];

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
      text: 'Share to GDrive',
      icon: 'add_to_drive',
      sortOrder: 10,
      badge: 'preview',
      visible: () => trace.isInternalUser,
      action: () => {
        // This button opens a modal dialog which displays the state of the file
        // in google drive allowing the user to upload it and optionally share
        // it. The dialog gets the state of whether the file is already in
        // google drive from the trace type + the fileId.
        showModal({
          key: 'GDriveUpload',
          title: 'Google Drive',
          content: () => m(UploadTraceModal, {trace}),
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
  const client = await getGDriveClient();
  const fileResult = await client.openFile(token, fileId);
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

  const client = await getGDriveClient();
  const auth = await client.authenticate();
  if (auth.response === 'popup_blocked') {
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
              const files = await client.requestFileAccess(accessToken, fileId);
              if (!files) return;
              await openGoogleDriveTrace(app, accessToken, fileId);
              closeModal('GoogleDriveAuthNeeded');
              // TODO(stevegolton): If this didn't work, I'm out of ideas!
            },
          }),
        ]),
    });
  }
}

interface UploadTraceModalAttrs {
  readonly trace: TraceImpl;
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
                const client = await getGDriveClient();
                const auth = await client.authenticate();
                if (auth.response === 'success') {
                  client.openSharingDialog(auth.accessToken, fileId);
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
                  const client = await getGDriveClient();
                  const auth = await client.authenticate();
                  if (auth.response !== 'success') return;
                  const folder = await client.pickFolder(auth.accessToken);
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
              const client = await getGDriveClient();
              const auth = await client.authenticate();
              if (auth.response !== 'success') return;
              const traceBlob = await trace.getTraceFile();
              const result = await client.uploadFile(
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
