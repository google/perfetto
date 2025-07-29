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

import {assertExists} from '../../base/logging';
import {errResult, okResult, Result} from '../../base/result';

// eslint-disable-next-line @typescript-eslint/no-explicit-any
type TokenClient = any;

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
              resolve({docs: assertExists(data.docs)});
              debugger;
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
    token: string,
    fileId: string,
  ): Promise<Result<GoogleDriveFile>> {
    console.log(token);

    try {
      const response = await gapi.client.drive.files.get({
        fileId: fileId,
        alt: 'media',
      });

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
        name: 'Google Drive Trace',
        blob: new Blob([buffer]),
      });
    } catch (error) {
      return errResult(error);
    }
  }

  async openSharingDialog(token: string, fileId: string) {
    await this.gapiLoad('drive-share');

    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const s = new (gapi as any).drive.share.ShareClient();
    s.setOAuthToken(token);
    s.setItemIds([fileId]);
    s.showSettingsDialog();
  }

  async uploadFile(
    token: string,
    traceBlob: Blob,
    parentId: string = 'root',
    fileName: string,
  ): Promise<Result<string>> {
    console.log('Uploading file to Google Drive:', {fileName, parentId});
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
      console.log('File uploaded successfully:', result);
      return okResult(fileId);
    } catch (error) {
      console.error('Error uploading file:', error);
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
              console.log('Using cached Google Drive token');
              // We need to do this when reusing a cached token, but not when
              // getting a new one. Maybe the tokenClient does this automatically
              // under the hood?
              gapi.client.setToken({access_token: cachedToken});
              resolve({response: 'success', accessToken: cachedToken});
              return;
            } else {
              localStorage.removeItem('driveToken');
              console.log(
                'Cached Google Drive token is invalid, fetching new one',
              );
            }
          }

          // HACK: The 'google.accounts' object is not available in the type definitions
          // until '@types/google-one-tap' is installed. We cast to 'any' to bypass
          // the type checker.
          const tokenClient: TokenClient =
            // eslint-disable-next-line @typescript-eslint/no-explicit-any
            (window as any).google.accounts.oauth2.initTokenClient({
              client_id: this.clientId,
              scope: SCOPES,
              // TODO(stevegolton): Add proper types once @types/google-one-tap is installed.
              // eslint-disable-next-line @typescript-eslint/no-explicit-any
              callback: (tokenResponse: any) => {
                console.log('Token response', tokenResponse);
                if (Boolean(tokenResponse.error)) {
                  console.error('OAuth Error:', tokenResponse.error);
                  return;
                }
                const accessToken = tokenResponse.access_token;
                // Resolve any pending promises waiting for the new token.
                if (accessToken != null) {
                  localStorage.setItem('driveToken', accessToken);
                  resolve({response: 'success', accessToken: accessToken});
                }
              },
              // eslint-disable-next-line @typescript-eslint/no-explicit-any
              error_callback: (foo: any) => {
                if (foo.type === 'popup_failed_to_open') {
                  resolve({response: 'popup_blocked'});
                } else if (foo.type === 'popup_closed') {
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
    return await new Promise((resolve) => {
      gapi.load(what, () => {
        resolve();
      });
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
    } catch (error) {
      console.error('Token validation error:', error);
      return false;
    }
  }
}
