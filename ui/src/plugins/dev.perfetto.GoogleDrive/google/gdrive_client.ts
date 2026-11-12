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

import {loadScript} from '../../../base/dom_utils';
import {errResult, okResult, type Result} from '../../../base/result';
import {loadGapiModules} from './gapi';

// The Google API loader script, loaded once by GDriveClient.create().
const GAPI_LOADER_URL = 'https://apis.google.com/js/api.js';

// The Drive REST API endpoint for files.
const DRIVE_FILES_URL = 'https://www.googleapis.com/drive/v3/files';

// How long to wait for each network step of GDriveClient.create() before
// giving up. Without this a stalled request would never settle.
const INIT_STEP_TIMEOUT_MS = 30000;

const DRIVE_ID_RE = /^[a-zA-Z0-9_-]+$/;

// Validates a Google Drive file ID or resource key against the allowed
// character set to guard against path traversal or header/query injection.
export function isValidDriveId(id: string): boolean {
  return DRIVE_ID_RE.test(id);
}

export interface GDriveClientConfig {
  readonly apiKey: string;
  readonly appId: string;
}

interface PickerConfig {
  // Builds the picker's view. Called inside picker()'s error handling, so that
  // a throw here is reported as an error rather than rejecting.
  readonly createView: () => google.picker.DocsView;
  readonly title?: string;
}

export interface PickerError {
  readonly type: 'cancelled' | 'invalid_id' | 'error';
  readonly cause?: unknown;
}

export interface DriveError {
  // 'access_denied' is true if Drive refused access to the file (HTTP 403 or
  // 404). Drive reports a file the app has not been granted access to as "not
  // found", so this also covers files that do not exist.
  readonly type: 'access_denied' | 'invalid_id' | 'error';
  readonly cause?: unknown;
}

function httpDriveError(status: number): DriveError {
  return {
    type: status === 403 || status === 404 ? 'access_denied' : 'error',
    cause: `Google Drive returned HTTP ${status}`,
  };
}

// Talks to Google Drive: shows its file picker and downloads files.
export class GDriveClient {
  private constructor(private readonly config: GDriveClientConfig) {}

  // Loads everything the client needs and returns a ready-to-use client:
  // Google's API loader and its picker module, for the file picker. Rejects if
  // any step fails.
  //
  // Google's API client (gapi.client) is deliberately not loaded: Drive
  // requests are made with plain fetch() and the access token.
  //
  // Nothing here opens a popup or needs a user gesture, so this can run at
  // startup.
  static async create(config: GDriveClientConfig): Promise<GDriveClient> {
    await loadScript(GAPI_LOADER_URL, INIT_STEP_TIMEOUT_MS);
    await loadGapiModules('picker', INIT_STEP_TIMEOUT_MS);
    return new GDriveClient(config);
  }

  // Open the Google Drive file picker to select a file. Never rejects.
  async pickFile(
    oauthToken: string,
  ): Promise<Result<google.picker.DocumentObject, PickerError>> {
    return this.picker(oauthToken, {
      createView: () =>
        new google.picker.DocsView(google.picker.ViewId.DOCS).setMode(
          google.picker.DocsViewMode.LIST,
        ),
    });
  }

  // Downloads a file's contents. Never rejects.
  async openFile(
    oauthToken: string,
    fileId: string,
  ): Promise<Result<ArrayBuffer, DriveError>> {
    // 'alt=media' asks for the file's contents rather than its metadata. The
    // response is read straight into an ArrayBuffer, avoiding extra copies of
    // large traces.
    return this.fetchFileResult(
      oauthToken,
      fileId,
      {alt: 'media'},
      async (response) => okResult(await response.arrayBuffer()),
    );
  }

  // Sends an authenticated GET for a file to the Drive REST API and parses the
  // response, returning a Result rather than rejecting.
  private async fetchFileResult<T>(
    oauthToken: string,
    fileId: string,
    params: Record<string, string>,
    parse: (response: Response) => Promise<Result<T, DriveError>>,
  ): Promise<Result<T, DriveError>> {
    if (!isValidDriveId(fileId)) {
      return errResult({
        type: 'invalid_id',
        cause: 'Invalid Google Drive file ID',
      });
    }
    try {
      const response = await this.fetchFile(oauthToken, fileId, params);
      if (!response.ok) {
        return errResult(httpDriveError(response.status));
      }
      return await parse(response);
    } catch (error) {
      return errResult({type: 'error', cause: error});
    }
  }

  // Sends an authenticated GET for a file to the Drive REST API.
  private async fetchFile(
    oauthToken: string,
    fileId: string,
    params: Record<string, string>,
  ): Promise<Response> {
    const url = new URL(`${DRIVE_FILES_URL}/${encodeURIComponent(fileId)}`);
    for (const [key, value] of Object.entries(params)) {
      url.searchParams.set(key, value);
    }
    return await fetch(url, {
      headers: {
        Authorization: `Bearer ${oauthToken}`,
      },
    });
  }

  private picker(
    oauthToken: string,
    config: PickerConfig,
  ): Promise<Result<google.picker.DocumentObject, PickerError>> {
    return new Promise<Result<google.picker.DocumentObject, PickerError>>(
      (resolve) => {
        try {
          const pickerBuilder = new google.picker.PickerBuilder()
            .setAppId(this.config.appId)
            .setOAuthToken(oauthToken)
            .setDeveloperKey(this.config.apiKey)
            .setOrigin(window.location.origin)
            .addView(config.createView())
            .setCallback((data) => {
              // Anything thrown in here would otherwise be lost, leaving the
              // promise pending for ever.
              try {
                switch (data.action) {
                  case google.picker.Action.PICKED: {
                    const file = data.docs?.[0];
                    if (file === undefined) {
                      resolve(errResult({type: 'cancelled'}));
                    } else {
                      resolve(okResult(file));
                    }
                    break;
                  }
                  case google.picker.Action.CANCEL:
                    resolve(errResult({type: 'cancelled'}));
                    break;
                  case google.picker.Action.ERROR:
                    resolve(
                      errResult({
                        type: 'error',
                        cause: new Error(
                          'Something went wrong with the picker',
                        ),
                      }),
                    );
                    break;
                }
              } catch (e) {
                resolve(errResult({type: 'error', cause: e}));
              }
            });

          if (config.title !== undefined) {
            pickerBuilder.setTitle(config.title);
          }

          const picker = pickerBuilder.build();
          picker.setVisible(true);
        } catch (e) {
          resolve(errResult({type: 'error', cause: e}));
        }
      },
    );
  }
}
