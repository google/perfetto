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
import type {AppImpl} from '../../core/app_impl';
import type {TraceImpl} from '../../core/trace_impl';
import type {PerfettoPlugin} from '../../public/plugin';
import {Button, ButtonVariant} from '../../widgets/button';
import {closeModal, showModal} from '../../widgets/modal';
import {Stack} from '../../widgets/stack';
import {GoogleDriveClient, type GoogleDriveFile} from './gdrive_client';
import type {Result} from '../../base/result';
import {defer} from '../../base/deferred';
import {Intent} from '../../widgets/common';
import {ShareForm} from './share_form';
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

interface GDriveConfigResult {
  readonly config: GDriveConfig;
  // Whether the config script was actually fetched (i.e. CONFIG_URL was
  // reachable). Used to decide whether to expose the sidebar button: the
  // config only exists on internal deployments.
  readonly ok: boolean;
}

let gDriveConfigPromise: Promise<GDriveConfigResult> | undefined;

// Kicks off the config script load and returns a promise that resolves to the
// populated window.gDriveConfig along with whether it was successfully
// fetched. The result is cached so the script is only loaded once.
function loadGDriveConfig(): Promise<GDriveConfigResult> {
  if (!gDriveConfigPromise) {
    const defaults: GDriveConfig = {apiKey: '', clientId: '', appId: ''};
    window.gDriveConfig = defaults;

    const loaded = defer<GDriveConfigResult>();
    // Only the first settle wins (onload/onerror/timeout can all fire).
    let settled = false;
    const settle = (ok: boolean) => {
      if (settled) return;
      settled = true;
      loaded.resolve({config: window.gDriveConfig ?? defaults, ok});
    };

    const script = document.createElement('script');
    script.src = CONFIG_URL;
    script.async = true;
    script.onload = () => settle(true);
    script.onerror = () => settle(false);
    document.head.append(script);

    setTimeout(() => settle(false), CONFIG_LOAD_TIMEOUT_MS);
    gDriveConfigPromise = loaded;
  }
  return gDriveConfigPromise;
}

let gDriveClientPromise: Promise<GoogleDriveClient> | undefined;

// Lazily loads the config and constructs the client on first use, caching the
// result so the config is only loaded once.
function getGDriveClient(): Promise<GoogleDriveClient> {
  if (!gDriveClientPromise) {
    gDriveClientPromise = loadGDriveConfig().then(
      ({config}) =>
        new GoogleDriveClient(config.apiKey, config.clientId, config.appId),
    );
  }
  return gDriveClientPromise;
}

const uploadedTraces = new WeakMap<TraceImpl, string>();
let pendingTraceFileId: string | undefined = undefined;

// The "Open with" deep link (?state=<json>) is handled by
// handleOpenWithState() below. For it to actually fire, the app still needs to
// be registered with Google Drive ("Open with" app) with the correct redirect
// URI, e.g. https://perfetto.dev/?state={...} where the JSON blob names the
// file to open. See TODO(stevegolton) for that setup.

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.GoogleDrive';
  static readonly description = 'Open and save traces to Google Drive';
  private location: google.picker.DocumentObject | 'root' = 'root';

  static onActivate(app: AppImpl) {
    // Only expose the sidebar button when the config is reachable (i.e. this
    // is an internal deployment). The config load is kicked off here and the
    // item is added once it resolves successfully; otherwise nothing is shown.
    loadGDriveConfig().then(({ok}) => {
      if (!ok) return;
      app.sidebar.addMenuItem({
        section: 'trace_files',
        text: 'Open from GDrive',
        icon: 'drive_export',
        badge: 'preview',
        // Just below "Open trace file".
        sortOrder: 1.25,
        action: async () => {
          const client = await getGDriveClient();
          const auth = await client.authenticate();
          if (auth.response !== 'success') return;
          const files = await client.pickFile(auth.accessToken);
          if (!files) return;
          if (files.length === 0) return;

          const firstFile = files[0];
          const fileResult = await client.openFile(
            firstFile.id,
            firstFile.name,
          );
          if (fileResult.ok) {
            const file = fileResult.value;
            pendingTraceFileId = file.id;
            app.openTraceFromBuffer({
              buffer: await file.blob.arrayBuffer(),
              title: file.name,
              fileName: file.name,
            });
          }
        },
      });
    });

    // When openining a trace directly from Google drive's UI, it passes the
    // file id and the resource keys in a hard-coded arg called 'state'.
    // Intercept this and load the file if these args are passed.
    const state = app.initialRouteArgs['state'];

    if (state !== undefined && typeof state === 'string') {
      handleOpenWithState(app, state);
    }

    // ?openFileId= share links, generated by the ShareForm.
    const openFileId = pluginArgs['openFileId'];

    if (openFileId !== undefined && typeof openFileId === 'string') {
      handleOpenFileId(app, openFileId);
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
      get visible() {
        return trace.isInternalUser;
      },
      action: () => {
        // This button opens a modal dialog which displays the state of the file
        // in google drive allowing the user to upload it and optionally share
        // it. The dialog gets the state of whether the file is already in
        // google drive from the trace type + the fileId.
        showModal({
          key: 'GDriveUpload',
          title: 'Share to Google Drive',
          icon: 'cloud_upload',
          content: () =>
            m(ShareForm, {
              initialFileName: trace.traceInfo.traceTitle ?? '',
              fileId: uploadedTraces.get(trace),
              location: this.location,
              onUpload: async (fileName: string, location: string) => {
                const client = await getGDriveClient();
                const auth = await client.authenticate();
                if (auth.response !== 'success') return;
                const traceBlob = await trace.getTraceFile();
                const result = await client.uploadFile(
                  auth.accessToken,
                  traceBlob,
                  location,
                  fileName,
                );
                if (result.ok) {
                  uploadedTraces.set(trace, result.value);
                }
              },
              onPickFolder: async () => {
                const client = await getGDriveClient();
                const auth = await client.authenticate();
                if (auth.response !== 'success') return;
                const newLocation = await client.pickFolder(auth.accessToken);
                if (newLocation !== undefined) {
                  this.location = newLocation;
                }
              },
              onOpenSharingDialog: async () => {
                const fileId = uploadedTraces.get(trace);
                if (!fileId) return;
                const client = await getGDriveClient();
                const auth = await client.authenticate();
                if (auth.response !== 'success') return;
                client.openSharingDialog(auth.accessToken, fileId);
              },
            }),
        });
      },
    });
  }
}

// The shape of the JSON blob Google Drive posts to our registered "Open with"
// redirect URL as ?state=<url-encoded-json> when a user opens a file in our
// app directly from the Drive UI.
interface GDriveOpenState {
  // The IDs of the file(s) being opened.
  readonly ids?: string[];
  // The action being performed; 'open' for the "Open with" flow.
  readonly action?: string;
  // Maps file id -> resource key. The resource key is required to access a
  // file via the Drive API when the app is not a collaborator on it.
  readonly resourceKeys?: Record<string, string>;
}

async function openGDriveTrace(
  app: AppImpl,
  fileId: string,
  resourceKey?: string,
): Promise<Result<GoogleDriveFile>> {
  const client = await getGDriveClient();
  const fileResult = await client.openFile(fileId, undefined, resourceKey);
  if (fileResult.ok) {
    const file = fileResult.value;
    app.openTraceFromBuffer({
      buffer: await file.blob.arrayBuffer(),
      title: file.name,
      fileName: file.name,
    });
    pendingTraceFileId = file.id;
  }
  return fileResult;
}

// Authenticate (handling the popup-blocked case) and open the trace, falling
// back to an "authorize file access" prompt if the initial fetch fails.
async function openGDriveTraceWithAuth(
  app: AppImpl,
  fileId: string,
  resourceKey?: string,
): Promise<void> {
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
          ]),
        ]),
    });
    return;
  }

  if (auth.response !== 'success') {
    return;
  }

  const accessToken = auth.accessToken;
  const fileResult = await openGDriveTrace(app, fileId, resourceKey);
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
              await openGDriveTrace(app, fileId, resourceKey);
              closeModal('GoogleDriveAuthNeeded');
            },
          }),
        ]),
    });
  }
}

// Handles the Google Drive "Open with" deep link (?state=<json>).
async function handleOpenWithState(app: AppImpl, state: string) {
  let parsed: GDriveOpenState;
  try {
    parsed = JSON.parse(state) as GDriveOpenState;
  } catch {
    // Not a GDrive "Open with" payload; ignore.
    return;
  }

  const fileId = parsed.ids?.[0];
  if (fileId === undefined) return;
  const resourceKey = parsed.resourceKeys?.[fileId];

  // Remove the 'state' query param now that we've consumed it. The router will
  // append ?local_cache_key=xxx to the hash once the trace finishes loading,
  // giving a clean shareable URL.
  const url = new URL(window.location.href);
  url.searchParams.delete('state');
  history.replaceState(null, '', url.toString());

  await openGDriveTraceWithAuth(app, fileId, resourceKey);
}

// Handles the ?openFileId= share link generated by the ShareForm.
async function handleOpenFileId(app: AppImpl, fileId: string) {
  await openGDriveTraceWithAuth(app, fileId);
}
