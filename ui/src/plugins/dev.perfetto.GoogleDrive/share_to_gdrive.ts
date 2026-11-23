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

import type {Trace} from '../../public/trace';
import {describeAuthFailure} from './dialogs';
import type {DriveClient, DriveFilePermission} from './google/drive_client';
import type {GoogleOAuthClient} from './google/oauth_client';
import type {
  SavedDomainShare,
  ShareState,
  UploadFolder,
  UploadState,
} from './views/share_form';

const DRIVE_FILE_SCOPE = 'https://www.googleapis.com/auth/drive.file';

// Custom MIME type registered under "Default MIME types" in the GCP project's
// Drive UI Integration settings so uploaded traces appear under "Open with
// Perfetto" in Drive regardless of their filename or extension.
const PERFETTO_TRACE_MIME_TYPE = 'application/vnd.perfetto.trace';

// The actions behind the buttons of the "Share to Google Drive" dialog. Each
// one starts from a click on its button, so the Google popup is allowed, and
// each is a whole flow, start to finish.
//
// Unlike the open flows these show no dialogs of their own, as that would
// replace the share dialog. Instead they record what happened in the trace's
// ShareState, which the dialog (and the sidebar button) render: a failure is
// recorded in `share.error` and shown in a banner, and the user retries by
// pressing the same button again. The caller redraws once each one finishes.

// Uploads the trace to Drive, under the name and into the folder chosen in the
// dialog. Progress and the outcome are recorded in `share`, so they are shown
// whenever the dialog is open, and on the sidebar button while it is closed.
export async function uploadTraceToGDrive(
  trace: Trace,
  oauth: GoogleOAuthClient,
  drive: DriveClient,
  share: ShareState,
): Promise<void> {
  // The dialog disables "Upload" in both of these cases.
  if (share.fileName === '' || share.upload !== undefined) return;

  // Fix the name, destination and default sharing now: the upload outlives
  // this click.
  const fileName = share.fileName;
  const parentId = share.location === 'root' ? 'root' : share.location.id;
  const domainsToShare = share.applyDefaultShareDomains
    ? share.defaultShareDomains
    : [];
  const upload: UploadState = {
    controller: new AbortController(),
    uploadedBytes: 0,
    totalBytes: 0,
  };
  const signal = upload.controller.signal;
  share.upload = upload;
  share.error = undefined;

  try {
    // Authenticate with the oauth client and obtain an access token. Nothing
    // is awaited before this call, so the popup is still attributed to the
    // click on "Upload".
    const auth = await oauth.authenticate([DRIVE_FILE_SCOPE]);
    if (!auth.ok) {
      share.error = {
        message: describeAuthFailure(auth.error),
        ...auth.error,
      };
      return;
    }

    // The user may have cancelled while the Google popup was open.
    if (signal.aborted) return;

    // Read back the trace's contents. The sidebar button is only shown for
    // traces where this is possible, but it can still fail.
    let traceBlob: Blob;
    try {
      traceBlob = await trace.getTraceFile();
    } catch (e) {
      share.error = {
        message:
          'This trace cannot be uploaded, as its contents are not available.',
        type: 'error',
        cause: e,
      };
      return;
    }

    // Upload it, reporting progress as it goes.
    const result = await drive.uploadFile(
      auth.value,
      traceBlob,
      parentId,
      fileName,
      {
        mimeType: PERFETTO_TRACE_MIME_TYPE,
        signal,
        onProgress: (uploadedBytes, totalBytes) => {
          upload.uploadedBytes = uploadedBytes;
          upload.totalBytes = totalBytes;
          trace.raf.scheduleFullRedraw();
        },
      },
    );
    if (!result.ok) {
      // Cancelling is the user's choice, not a failure.
      if (result.error.type === 'cancelled') return;
      share.error = {
        message: "Couldn't upload the trace to Google Drive.",
        ...result.error,
      };
      return;
    }

    // Apply remembered domain/audience permissions if enabled.
    // Note: we always set allowFileDiscovery: true as a workaround for a
    // Google Picker quirk where DocsView.setFileIds() shows an empty picker
    // for link-only (allowFileDiscovery: false) files until the recipient
    // opens the Drive URL directly once.
    for (const domainShare of domainsToShare) {
      const shareRes = await drive.createPermission(
        auth.value,
        result.value.id,
        {
          type: 'domain',
          domain: domainShare.domain,
          role: domainShare.role,
          allowFileDiscovery: true,
        },
      );
      if (!shareRes.ok) {
        share.error = {
          message:
            "Uploaded to Google Drive, but couldn't apply default sharing permissions.",
          ...shareRes.error,
        };
        break;
      }
    }

    // Fetch initial permissions for the newly uploaded file.
    const perms = await drive.getPermissions(
      auth.value,
      result.value.id,
      result.value.resourceKey,
    );
    if (perms.ok) {
      share.permissions = perms.value;
    }

    share.fileId = result.value.id;
    share.resourceKey = result.value.resourceKey;
  } finally {
    share.upload = undefined;
  }
}

export function extractDomainShares(
  permissions: ReadonlyArray<DriveFilePermission>,
): SavedDomainShare[] {
  return permissions
    .filter(
      (p): p is DriveFilePermission & {readonly domain: string} =>
        p.type === 'domain' && p.domain !== undefined,
    )
    .map((p) => ({
      domain: p.domain,
      displayName: p.displayName,
      role: p.role,
    }));
}

// Lets the user choose which Drive folder to upload to.
export async function pickUploadFolder(
  oauth: GoogleOAuthClient,
  drive: DriveClient,
  share: ShareState,
  onLocationChange?: (location: UploadFolder | 'root') => void,
): Promise<void> {
  share.error = undefined;

  // Authenticate with the oauth client and obtain an access token. Nothing is
  // awaited before this call, so the popup is still attributed to the click on
  // "Change".
  const auth = await oauth.authenticate([DRIVE_FILE_SCOPE]);
  if (!auth.ok) {
    share.error = {message: describeAuthFailure(auth.error), ...auth.error};
    return;
  }

  // Let the user choose a folder. Cancelling leaves the location as it was.
  const picked = await drive.pickFolder(auth.value);
  if (!picked.ok) {
    if (picked.error.type === 'cancelled') return;
    share.error = {
      message: "Couldn't open the Google Drive folder picker.",
      ...picked.error,
    };
    return;
  }

  const location: UploadFolder = {
    id: picked.value.id,
    name: picked.value.name,
  };
  share.location = location;
  onLocationChange?.(location);
}

// Opens Drive's sharing settings for the uploaded file.
export async function openGDriveSharingDialog(
  oauth: GoogleOAuthClient,
  drive: DriveClient,
  share: ShareState,
  onClose?: () => void,
): Promise<void> {
  const fileId = share.fileId;
  if (fileId === undefined || fileId === '') return;
  share.error = undefined;

  // Authenticate with the oauth client and obtain an access token. Nothing is
  // awaited before this call, so the popup is still attributed to the click on
  // "Share".
  const auth = await oauth.authenticate([DRIVE_FILE_SCOPE]);
  if (!auth.ok) {
    share.error = {message: describeAuthFailure(auth.error), ...auth.error};
    return;
  }

  // Show Drive's own sharing dialog.
  const result = drive.openSharingDialog(auth.value, fileId, {onClose});
  if (!result.ok) {
    share.error = {
      message: "Couldn't open the Google Drive sharing dialog.",
      ...result.error,
    };
  }
}

// Fetches the current sharing permissions for the file in Drive.
export async function refreshGDrivePermissions(
  oauth: GoogleOAuthClient,
  drive: DriveClient,
  share: ShareState,
  onPermissionsLoaded?: (
    permissions: ReadonlyArray<DriveFilePermission>,
  ) => void,
): Promise<void> {
  const fileId = share.fileId;
  if (fileId === undefined || fileId === '' || share.loadingPermissions) return;
  share.loadingPermissions = true;
  share.error = undefined;

  try {
    const auth = await oauth.authenticate([DRIVE_FILE_SCOPE]);
    if (!auth.ok) {
      share.error = {message: describeAuthFailure(auth.error), ...auth.error};
      return;
    }

    const result = await drive.getPermissions(
      auth.value,
      fileId,
      share.resourceKey,
    );
    if (!result.ok) {
      if (result.error.type === 'access_denied') {
        share.permissions = 'access_denied';
        return;
      }
      share.error = {
        message: "Couldn't load sharing permissions from Google Drive.",
        ...result.error,
      };
      return;
    }

    share.permissions = result.value;
    onPermissionsLoaded?.(result.value);
  } finally {
    share.loadingPermissions = false;
  }
}

export interface MakeSearchableOptions {
  readonly onPermissionsLoaded?: (
    permissions: ReadonlyArray<DriveFilePermission>,
  ) => void;
  // When true, failures are ignored instead of recording `share.error`
  // (best-effort update after closing the sharing dialog).
  readonly silent?: boolean;
}

// Enables search discovery (allowFileDiscovery: true) on any existing
// link-only domain permissions on the file, then refreshes permissions.
// Deliberately skips `type: 'anyone'` so link-shared files are never turned
// into publicly searchable web files.
export async function makeGDrivePermissionsSearchable(
  oauth: GoogleOAuthClient,
  drive: DriveClient,
  share: ShareState,
  options: MakeSearchableOptions = {},
): Promise<void> {
  const {onPermissionsLoaded, silent = false} = options;
  const fileId = share.fileId;
  if (fileId === undefined || fileId === '' || share.loadingPermissions) return;
  const unsearchable =
    share.permissions !== undefined && share.permissions !== 'access_denied'
      ? share.permissions.filter(
          (p): p is DriveFilePermission & {readonly domain: string} =>
            p.type === 'domain' &&
            p.domain !== undefined &&
            p.allowFileDiscovery !== true,
        )
      : [];
  if (unsearchable.length === 0) return;

  share.loadingPermissions = true;
  if (!silent) {
    share.error = undefined;
  }

  try {
    const auth = await oauth.authenticate([DRIVE_FILE_SCOPE]);
    if (!auth.ok) {
      if (!silent) {
        share.error = {message: describeAuthFailure(auth.error), ...auth.error};
      }
      return;
    }

    let anyUpdated = false;
    for (const perm of unsearchable) {
      const res = await drive.createPermission(auth.value, fileId, {
        type: 'domain',
        domain: perm.domain,
        role: perm.role,
        allowFileDiscovery: true,
      });
      if (!res.ok) {
        if (!silent) {
          share.error = {
            message: "Couldn't make this trace searchable in Google Drive.",
            ...res.error,
          };
          return;
        }
        continue;
      }
      anyUpdated = true;
    }

    if (!anyUpdated) return;

    const updated = await drive.getPermissions(
      auth.value,
      fileId,
      share.resourceKey,
    );
    if (updated.ok) {
      share.permissions = updated.value;
      onPermissionsLoaded?.(updated.value);
    }
  } finally {
    share.loadingPermissions = false;
  }
}
