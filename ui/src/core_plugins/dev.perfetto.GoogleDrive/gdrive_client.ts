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

import {ensureExists} from '../../base/assert';
import {errResult, okResult, type Result} from '../../base/result';

// The Google Drive / Google Identity Services loader scripts. We load these
// lazily (rather than statically in index.html) so that only users who
// actually use the plugin pay the cost of fetching them.
const GAPI_LOADER_URL = 'https://apis.google.com/js/api.js';
const GSI_CLIENT_URL = 'https://accounts.google.com/gsi/client';

// Loads an external <script> and resolves once it has finished loading. The
// promise is cached per URL so that repeated calls share a single load.
const scriptLoadPromises = new Map<string, Promise<void>>();
function loadScript(src: string): Promise<void> {
  let promise = scriptLoadPromises.get(src);
  if (promise === undefined) {
    promise = new Promise<void>((resolve, reject) => {
      const script = document.createElement('script');
      script.src = src;
      script.async = true;
      script.onload = () => resolve();
      script.onerror = () => reject(new Error(`Failed to load ${src}`));
      document.head.append(script);
    });
    scriptLoadPromises.set(src, promise);
  }
  return promise;
}

export const SCOPES =
  'https://www.googleapis.com/auth/drive.file https://www.googleapis.com/auth/drive.install';

export interface PickerResponse {
  readonly docs: google.picker.DocumentObject[];
}

export interface PickerConfig {
  readonly view: google.picker.DocsView;
  readonly title?: string;
}

export interface GoogleDriveFile {
  readonly id: string;
  readonly name: string;
  readonly blob: Blob;
}

export type AuthenticationResponse =
  | {
      readonly response: 'success';
      readonly accessToken: string;
    }
  | {
      readonly response: 'popup_blocked';
    }
  | {
      readonly response: 'popup_closed';
    };

export class GoogleDriveClient {
  private clientInitPromise: Promise<void> | undefined;
  private pendingToken: Promise<AuthenticationResponse> | undefined;

  constructor(
    private readonly apiKey: string,
    private readonly clientId: string,
    private readonly appId: string,
  ) {}

  async picker(
    token: string,
    config: PickerConfig,
  ): Promise<PickerResponse | undefined> {
    return await new Promise<PickerResponse | undefined>((resolve, reject) => {
      const pickerBuilder = new google.picker.PickerBuilder()
        .setAppId(this.appId)
        .setOAuthToken(token)
        .setDeveloperKey(this.apiKey)
        .setOrigin(window.location.protocol + '//' + window.location.host)
        .addView(config.view)
        .setCallback((data) => {
          switch (data.action) {
            case google.picker.Action.PICKED:
              resolve({docs: ensureExists(data.docs)});
              break;
            case google.picker.Action.CANCEL:
              resolve(undefined);
              break;
            case google.picker.Action.ERROR:
              reject(new Error('Something went wrong with the picker'));
              break;
          }
        });

      if (config.title) {
        pickerBuilder.setTitle(config.title);
      }

      const picker = pickerBuilder.build();
      picker.setVisible(true);
    });
  }

  // Request access to a specific file by ID via the picker API.
  async requestFileAccess(
    token: string,
    fileId: string,
  ): Promise<google.picker.DocumentObject[] | undefined> {
    await this.gapiLoad('picker');
    const view = new google.picker.DocsView(google.picker.ViewId.DOCS)
      .setMode(google.picker.DocsViewMode.LIST)
      .setFileIds(fileId);
    const pickerResult = await this.picker(token, {view});
    return pickerResult?.docs;
  }

  // Open the Google Drive file picker to select a file.
  async pickFile(
    token: string,
  ): Promise<google.picker.DocumentObject[] | undefined> {
    await this.gapiLoad('picker');
    const view = new google.picker.DocsView(google.picker.ViewId.DOCS).setMode(
      google.picker.DocsViewMode.LIST,
    );
    const pickerResult = await this.picker(token, {view});
    return pickerResult?.docs;
  }

  // Pick a folder using the Google Drive file picker.
  async pickFolder(
    token: string,
  ): Promise<google.picker.DocumentObject | undefined> {
    await this.gapiLoad('picker');
    const view = new google.picker.DocsView(google.picker.ViewId.FOLDERS)
      .setMode(google.picker.DocsViewMode.LIST)
      .setIncludeFolders(true)
      .setSelectFolderEnabled(true)
      .setMimeTypes('application/vnd.google-apps.folder');
    const pickerResult = await this.picker(token, {
      view,
      title: 'Select folder',
    });
    return pickerResult?.docs[0]!;
  }

  async openFile(
    fileId: string,
    name?: string,
    resourceKey?: string,
  ): Promise<Result<GoogleDriveFile>> {
    try {
      // The resource key (from the "Open with" state payload) is required to
      // access a file via the Drive API when the app is not a collaborator.
      const params: {fileId: string; alt: 'media'; key?: string} = {
        fileId: fileId,
        alt: 'media',
      };
      if (resourceKey !== undefined) {
        params.key = resourceKey;
      }
      const response = await gapi.client.drive.files.get(params);

      // The response body is a string, but it represents binary data.
      // We can convert it to an ArrayBuffer by accessing the character codes.
      const body = response.body;
      const buffer = new ArrayBuffer(body.length);
      const view = new Uint8Array(buffer);
      for (let i = 0; i < body.length; i++) {
        view[i] = body.charCodeAt(i);
      }
      return okResult({
        id: fileId,
        name: name ?? 'Google Drive Trace',
        blob: new Blob([buffer]),
      });
    } catch (error) {
      return errResult(error);
    }
  }

  async openSharingDialog(token: string, fileId: string) {
    await this.gapiLoad('drive-share');

    const shareClient = new gapi.drive.share.ShareClient();
    shareClient.setOAuthToken(token);
    shareClient.setItemIds([fileId]);
    shareClient.showSettingsDialog();
  }

  async uploadFile(
    token: string,
    traceBlob: Blob,
    parentId: string = 'root',
    fileName: string,
  ): Promise<Result<string>> {
    const traceBuffer = await traceBlob.arrayBuffer();

    const metadata = {
      name: fileName,
      mimeType: 'application/octet-stream',
      parents: [parentId],
    };

    const boundary = '-------314159265358979323846';
    const delimiter = `\r\n--${boundary}\r\n`;
    const closeDelim = `\r\n--${boundary}--`;

    const metadataPart =
      delimiter +
      'Content-Type: application/json; charset=UTF-8\r\n\r\n' +
      JSON.stringify(metadata);

    const mediaPart =
      delimiter + 'Content-Type: application/octet-stream\r\n\r\n';

    const body = new Blob([
      new TextEncoder().encode(metadataPart),
      new TextEncoder().encode(mediaPart),
      traceBuffer,
      new TextEncoder().encode(closeDelim),
    ]);

    try {
      const response = await fetch(
        'https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart',
        {
          method: 'POST',
          headers: {
            'Content-Type': `multipart/related; boundary=${boundary}`,
            'Authorization': `Bearer ${token}`,
          },
          body,
        },
      );
      const result = await response.json();
      const fileId = result.id;
      return okResult(fileId);
    } catch {
      return errResult('Upload failed');
    }
  }

  async authenticate(): Promise<AuthenticationResponse> {
    await this.maybeInitClient();

    if (!this.pendingToken) {
      this.pendingToken = new Promise<AuthenticationResponse>(
        async (resolve, reject) => {
          const cachedToken = localStorage.getItem('driveToken');
          if (cachedToken !== null) {
            if (await this.isTokenValid(cachedToken)) {
              // We need to do this when reusing a cached token, but not when
              // getting a new one. Maybe the tokenClient does this automatically
              // under the hood?
              gapi.client.setToken({access_token: cachedToken});
              resolve({response: 'success', accessToken: cachedToken});
              return;
            } else {
              localStorage.removeItem('driveToken');
            }
          }

          // The google.accounts.oauth2 token client is typed via the Window
          // augmentation in google_drive.d.ts. Load the GSI client script
          // lazily before using it.
          await loadScript(GSI_CLIENT_URL);
          const oauth2 = window.google?.accounts?.oauth2;
          if (!oauth2) {
            reject(new Error('Google Identity Services is not available'));
            return;
          }
          const tokenClient: GDriveTokenClient = oauth2.initTokenClient({
            client_id: this.clientId,
            scope: SCOPES,
            callback: (tokenResponse: GDriveTokenResponse) => {
              if (Boolean(tokenResponse.error)) {
                return;
              }
              const accessToken = tokenResponse.access_token;
              // Resolve any pending promises waiting for the new token.
              if (accessToken != null) {
                localStorage.setItem('driveToken', accessToken);
                resolve({response: 'success', accessToken: accessToken});
              }
            },
            error_callback: (error: GDriveTokenClientError) => {
              if (error.type === 'popup_failed_to_open') {
                resolve({response: 'popup_blocked'});
              } else if (error.type === 'popup_closed') {
                resolve({response: 'popup_closed'});
              } else {
                // Improve this message
                reject(new Error('Something went wrong'));
              }
              // Clear the pending promise so that future calls to
              // authenticate() can try again.
              this.pendingToken = undefined;
            },
          });
          tokenClient.requestAccessToken({prompt: ''});
        },
      );
    }

    return await this.pendingToken;
  }

  private async maybeInitClient(): Promise<void> {
    if (!this.clientInitPromise) {
      this.clientInitPromise = new Promise<void>(async (resolve) => {
        await this.gapiLoad('client');
        await gapi.client.init({
          apiKey: this.apiKey,
          discoveryDocs: [
            'https://www.googleapis.com/discovery/v1/apis/drive/v3/rest',
          ],
        });
        resolve();
      });
    }
    await this.clientInitPromise;
  }

  private async gapiLoad(what: string): Promise<void> {
    await loadScript(GAPI_LOADER_URL);
    await new Promise<void>((resolve) => {
      gapi.load(what, () => resolve());
    });
  }

  private async isTokenValid(token: string) {
    try {
      const response = await fetch(
        'https://www.googleapis.com/oauth2/v1/tokeninfo',
        {
          method: 'GET',
          headers: {
            Authorization: `Bearer ${token}`,
          },
        },
      );

      return response.ok;
    } catch {
      return false;
    }
  }
}
