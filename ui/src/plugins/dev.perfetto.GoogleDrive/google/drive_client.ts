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

import {z} from 'zod';
import {assertExists, assertNonEmpty} from '../../../base/assert';
import {defer} from '../../../base/deferred';
import {loadScript} from '../../../base/dom_utils';
import {errResult, okResult, type Result} from '../../../base/result';
import {loadGapiModules} from './gapi';

// The Google API loader script, loaded once by DriveClient.create().
const GAPI_LOADER_URL = 'https://apis.google.com/js/api.js';

// The Drive REST API endpoint for files.
const DRIVE_FILES_URL = 'https://www.googleapis.com/drive/v3/files';

// The Drive REST API endpoint for uploading a file with its metadata.
// supportsAllDrives lets the destination folder be in a shared drive; without
// it Drive reports such a folder as not found.
const DRIVE_UPLOAD_URL =
  'https://www.googleapis.com/upload/drive/v3/files' +
  '?uploadType=multipart&supportsAllDrives=true';

// How long to wait for each network step of DriveClient.create() before
// giving up. Without this a stalled request would never settle.
const INIT_STEP_TIMEOUT_MS = 30000;

const DRIVE_ID_RE = /^[a-zA-Z0-9_-]+$/;

// Validates a Google Drive file ID or resource key against the allowed
// character set to guard against path traversal or header/query injection.
export function isValidDriveId(id: string): boolean {
  return DRIVE_ID_RE.test(id);
}

export interface DriveClientConfig {
  readonly apiKey: string;
  readonly appId: string;
}

interface PickerConfig {
  readonly view: google.picker.DocsView;
  readonly title?: string;
}

export interface PickerError {
  readonly type: 'cancelled' | 'invalid_id' | 'error';
  readonly cause?: unknown;
}

export type PickerResult = Result<google.picker.DocumentObject, PickerError>;

export interface GoogleDriveFileMeta {
  readonly name: string;
}

const FILE_META_SCHEMA = z.object({name: z.string()});

// The resource key (from the "Open with" state payload) is required to access
// some link-shared files the user has not opened before. Drive expects it in
// the X-Goog-Drive-Resource-Keys header as "<fileId>/<resourceKey>".
function resourceKeyHeaders(
  fileId: string,
  resourceKey: string | undefined,
): Record<string, string> {
  if (resourceKey === undefined) return {};
  return {'X-Goog-Drive-Resource-Keys': `${fileId}/${resourceKey}`};
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

// What Drive returns for a successful upload: the new file's record.
const UPLOAD_RESPONSE_SCHEMA = z.object({id: z.string()});

export interface UploadOptions {
  // Called as the upload progresses, with the bytes sent so far and the total
  // to send. Both include a small amount of framing around the file itself.
  readonly onProgress?: (uploadedBytes: number, totalBytes: number) => void;
  // Abort this to cancel the upload.
  readonly signal?: AbortSignal;
}

export interface UploadError {
  // 'cancelled' if the upload was cancelled through UploadOptions.signal.
  readonly type: 'cancelled' | 'error';
  readonly cause?: unknown;
}

export interface SharingDialogError {
  readonly type: 'error';
  readonly cause?: unknown;
}

// The body of an error response from the Drive API.
const DRIVE_ERROR_SCHEMA = z.object({error: z.object({message: z.string()})});

// Extracts Drive's explanation from the body of an error response, if it has
// one.
function driveErrorMessage(responseText: string): string | undefined {
  let json: unknown;
  try {
    json = JSON.parse(responseText);
  } catch {
    return undefined;
  }
  const result = DRIVE_ERROR_SCHEMA.safeParse(json);
  return result.success ? result.data.error.message : undefined;
}

// Describes an upload that Drive answered with an error status.
function uploadHttpError(status: number, responseText: string): UploadError {
  // Drive explains the failure (e.g. storage quota exceeded) in the response
  // body; pass that on if it is there.
  const reason = driveErrorMessage(responseText);
  return {
    type: 'error',
    cause:
      reason === undefined
        ? `Google Drive returned HTTP ${status}`
        : `${reason} (HTTP ${status})`,
  };
}

// Reads the new file's ID out of Drive's response to a successful upload.
function parseUploadedFileId(
  responseText: string,
): Result<string, UploadError> {
  let json: unknown;
  try {
    json = JSON.parse(responseText);
  } catch {
    json = undefined;
  }
  const result = UPLOAD_RESPONSE_SCHEMA.safeParse(json);
  if (!result.success) {
    return errResult({
      type: 'error',
      cause: 'Google Drive did not return a file ID',
    });
  }
  return okResult(result.data.id);
}

// The body of a Drive "multipart" upload, which sends a file's metadata and
// its contents in one request, and the Content-Type header to send it with.
interface MultipartUpload {
  readonly body: Blob;
  readonly contentType: string;
}

function buildMultipartUpload(
  metadata: {readonly [key: string]: unknown},
  media: Blob,
): MultipartUpload {
  const boundary = '-------314159265358979323846';
  const delimiter = `\r\n--${boundary}\r\n`;
  const closeDelim = `\r\n--${boundary}--`;

  const metadataPart =
    delimiter +
    'Content-Type: application/json; charset=UTF-8\r\n\r\n' +
    JSON.stringify(metadata);

  const mediaPart =
    delimiter + 'Content-Type: application/octet-stream\r\n\r\n';

  return {
    // The media is referenced by the body rather than copied into it.
    body: new Blob([metadataPart, mediaPart, media, closeDelim]),
    contentType: `multipart/related; boundary=${boundary}`,
  };
}

// What the server sent back for a request that completed.
interface PostResponse {
  readonly status: number;
  readonly responseText: string;
}

// POSTs `body` to `url`, resolving with the server's response whatever its
// status. Never rejects: a request that could not be completed, or was
// cancelled, resolves with an error.
//
// This uses XMLHttpRequest rather than fetch(), as fetch() cannot report
// upload progress.
function postWithProgress(
  url: string,
  headers: {readonly [name: string]: string},
  body: Blob,
  opts?: UploadOptions,
): Promise<Result<PostResponse, UploadError>> {
  const deferred = defer<Result<PostResponse, UploadError>>();

  const signal = opts?.signal;
  if (signal?.aborted) {
    deferred.resolve(errResult({type: 'cancelled'}));
    return deferred;
  }

  try {
    const req = new XMLHttpRequest();
    req.open('POST', url, /* async= */ true);
    for (const [name, value] of Object.entries(headers)) {
      req.setRequestHeader(name, value);
    }

    req.upload.onprogress = (e: ProgressEvent) => {
      opts?.onProgress?.(e.loaded, e.total);
    };
    req.onabort = () => deferred.resolve(errResult({type: 'cancelled'}));
    req.onerror = () =>
      deferred.resolve(errResult({type: 'error', cause: 'Network error'}));
    req.onload = () =>
      deferred.resolve(
        okResult({status: req.status, responseText: req.responseText}),
      );

    signal?.addEventListener('abort', () => req.abort(), {once: true});
    req.send(body);
  } catch (e) {
    deferred.resolve(errResult({type: 'error', cause: e}));
  }

  return deferred;
}

// Talks to Google Drive: shows its file and folder pickers and sharing dialog,
// and downloads and uploads files.
export class DriveClient {
  private constructor(private readonly config: DriveClientConfig) {}

  // Loads everything the client needs and returns a ready-to-use client:
  // Google's API loader with its picker and drive-share modules, for the
  // pickers and the sharing dialog. Rejects if any step fails.
  //
  // Google's API client (gapi.client) is deliberately not loaded: Drive
  // requests are made with plain fetch() and the access token.
  //
  // Nothing here opens a popup or needs a user gesture, so this can run at
  // startup.
  static async create(config: DriveClientConfig): Promise<DriveClient> {
    await loadScript(GAPI_LOADER_URL, INIT_STEP_TIMEOUT_MS);
    await loadGapiModules('picker:drive-share', INIT_STEP_TIMEOUT_MS);
    return new DriveClient(config);
  }

  // Request access to a specific file by ID via the picker API.
  async requestFileAccess(
    oauthToken: string,
    fileId: string,
  ): Promise<Result<google.picker.DocumentObject, PickerError>> {
    if (!isValidDriveId(fileId)) {
      return errResult({
        type: 'invalid_id',
        cause: 'Invalid Google Drive file ID',
      });
    }
    const view = new google.picker.DocsView(google.picker.ViewId.DOCS);
    view.setMode(google.picker.DocsViewMode.LIST);
    view.setFileIds(fileId);
    return this.picker(oauthToken, {view});
  }

  // Open the Google Drive file picker to select a file.
  async pickFile(oauthToken: string): Promise<PickerResult> {
    const view = new google.picker.DocsView(google.picker.ViewId.DOCS);
    view.setMode(google.picker.DocsViewMode.LIST);
    return this.picker(oauthToken, {view});
  }

  // Open the Google Drive picker to select a folder.
  async pickFolder(
    oauthToken: string,
  ): Promise<Result<google.picker.DocumentObject, PickerError>> {
    const view = new google.picker.DocsView(google.picker.ViewId.FOLDERS);
    view.setMode(google.picker.DocsViewMode.LIST);
    view.setIncludeFolders(true);
    view.setSelectFolderEnabled(true);
    view.setMimeTypes('application/vnd.google-apps.folder');
    return this.picker(oauthToken, {view, title: 'Select folder'});
  }

  // Opens Drive's sharing settings dialog for a file. The drive-share module
  // it needs is loaded by create(). Never throws.
  openSharingDialog(
    oauthToken: string,
    fileId: string,
  ): Result<void, SharingDialogError> {
    try {
      const shareClient = new gapi.drive.share.ShareClient();
      shareClient.setOAuthToken(oauthToken);
      shareClient.setItemIds([fileId]);
      shareClient.showSettingsDialog();
      return okResult();
    } catch (e) {
      return errResult({type: 'error', cause: e});
    }
  }

  // Uploads a file to Drive, resolving with the new file's ID. Never rejects.
  //
  // `opts.onProgress` is called as bytes are sent, and aborting `opts.signal`
  // cancels the upload.
  async uploadFile(
    oauthToken: string,
    traceBlob: Blob,
    parentId: string,
    fileName: string,
    opts?: UploadOptions,
  ): Promise<Result<string, UploadError>> {
    const {body, contentType} = buildMultipartUpload(
      {
        name: fileName,
        mimeType: 'application/octet-stream',
        parents: [parentId],
      },
      traceBlob,
    );

    const response = await postWithProgress(
      DRIVE_UPLOAD_URL,
      {
        'Content-Type': contentType,
        'Authorization': `Bearer ${oauthToken}`,
      },
      body,
      opts,
    );
    if (!response.ok) return response;

    const {status, responseText} = response.value;
    if (status < 200 || status >= 300) {
      return errResult(uploadHttpError(status, responseText));
    }
    return parseUploadedFileId(responseText);
  }

  // Downloads a file's contents. Never rejects.
  async openFile(
    oauthToken: string,
    fileId: string,
    resourceKey?: string,
  ): Promise<Result<ArrayBuffer, DriveError>> {
    // 'alt=media' asks for the file's contents rather than its metadata. The
    // response is read straight into an ArrayBuffer, avoiding extra copies of
    // large traces.
    return this.fetchFileResult(
      oauthToken,
      fileId,
      {alt: 'media'},
      resourceKey,
      async (response) => okResult(await response.arrayBuffer()),
    );
  }

  // Fetches a file's metadata (currently just its name). Never rejects.
  async getMeta(
    oauthToken: string,
    fileId: string,
    resourceKey?: string,
  ): Promise<Result<GoogleDriveFileMeta, DriveError>> {
    return this.fetchFileResult(
      oauthToken,
      fileId,
      {fields: 'name'},
      resourceKey,
      async (response) => {
        const meta = FILE_META_SCHEMA.safeParse(await response.json());
        if (!meta.success) {
          return errResult({type: 'error', cause: meta.error});
        }
        return okResult(meta.data);
      },
    );
  }

  // Sends an authenticated GET for a file to the Drive REST API and parses the
  // response, returning a Result rather than rejecting.
  private async fetchFileResult<T>(
    oauthToken: string,
    fileId: string,
    params: Record<string, string>,
    resourceKey: string | undefined,
    parse: (response: Response) => Promise<Result<T, DriveError>>,
  ): Promise<Result<T, DriveError>> {
    if (!isValidDriveId(fileId)) {
      return errResult({
        type: 'invalid_id',
        cause: 'Invalid Google Drive file ID',
      });
    }
    if (resourceKey !== undefined && !isValidDriveId(resourceKey)) {
      return errResult({
        type: 'invalid_id',
        cause: 'Invalid Google Drive resource key',
      });
    }
    try {
      const response = await this.fetchFile(
        oauthToken,
        fileId,
        params,
        resourceKey,
      );
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
    resourceKey: string | undefined,
  ): Promise<Response> {
    const url = new URL(`${DRIVE_FILES_URL}/${encodeURIComponent(fileId)}`);
    for (const [key, value] of Object.entries(params)) {
      url.searchParams.set(key, value);
    }
    return await fetch(url, {
      headers: {
        Authorization: `Bearer ${oauthToken}`,
        ...resourceKeyHeaders(fileId, resourceKey),
      },
    });
  }

  // Opens the Google Drive Picker in an iframe, returning the picked file or an
  // error. This allows the user to browse and select a file from their Google
  // Drive without leaving the current page nor requiring Perfetto access to the
  // file list.
  private picker(
    oauthToken: string,
    config: PickerConfig,
  ): Promise<PickerResult> {
    const deferred = defer<PickerResult>();

    function pickerCallback(data: google.picker.ResponseObject) {
      switch (data.action) {
        case google.picker.Action.PICKED: {
          assertExists(data.docs);
          assertNonEmpty(data.docs);
          const [firstDoc] = data.docs;
          deferred.resolve(okResult(firstDoc));
          break;
        }
        case google.picker.Action.CANCEL:
          deferred.resolve(errResult({type: 'cancelled'}));
          break;
        case google.picker.Action.ERROR:
          deferred.resolve(
            errResult({
              type: 'error',
              cause: new Error('Something went wrong with the picker'),
            }),
          );
          break;
        default:
          // Ignore other actions (e.g., 'loaded')
          break;
      }
    }

    try {
      const pickerBuilder = new google.picker.PickerBuilder()
        .setAppId(this.config.appId)
        .setOAuthToken(oauthToken)
        .setDeveloperKey(this.config.apiKey)
        .setOrigin(window.location.origin)
        .addView(config.view)
        .setCallback(pickerCallback);

      if (config.title !== undefined) {
        pickerBuilder.setTitle(config.title);
      }

      pickerBuilder.build().setVisible(true);
    } catch (e) {
      deferred.resolve(errResult({type: 'error', cause: e}));
    }

    return deferred;
  }
}

// Google Drive's "Open with" deep link. When a user opens a file with Perfetto
// from the Drive UI, Drive loads our registered URL with ?state=<json> naming
// the file. This relies on the app being registered with Google Drive as an
// "Open with" app, with a redirect URI such as
// https://perfetto.dev/?state={...}.

// The shape of the JSON blob Google Drive passes as ?state=<url-encoded-json>.
const GDRIVE_OPEN_STATE_SCHEMA = z.object({
  // The IDs of the file(s) being opened.
  ids: z.array(z.string()).optional(),
  // The action being performed; 'open' for the "Open with" flow.
  action: z.string().optional(),
  // Maps file id -> resource key. The resource key is required to access a
  // file via the Drive API when the app is not a collaborator on it.
  resourceKeys: z.record(z.string(), z.string()).optional(),
});

export interface GDriveOpenFileRequest {
  readonly fileId: string;
  readonly resourceKey?: string;
}

export function parseGDriveStateParam(
  stateParam: string,
): Result<GDriveOpenFileRequest> {
  let raw: unknown;
  try {
    raw = JSON.parse(stateParam);
  } catch {
    // Not a GDrive "Open with" payload; ignore.
    return errResult('Invalid JSON');
  }

  const parsed = GDRIVE_OPEN_STATE_SCHEMA.safeParse(raw);
  if (!parsed.success) {
    return errResult('Invalid GDrive state');
  }

  const fileId = parsed.data.ids?.[0];

  if (!fileId) {
    return errResult('No file IDs provided');
  }

  if (!isValidDriveId(fileId)) {
    return errResult('Invalid file ID');
  }

  const resourceKey = parsed.data.resourceKeys?.[fileId];

  if (resourceKey !== undefined && !isValidDriveId(resourceKey)) {
    return errResult('Invalid resource key');
  }

  return okResult({fileId, resourceKey});
}
