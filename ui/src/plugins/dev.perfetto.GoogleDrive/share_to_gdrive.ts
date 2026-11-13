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
import type {DriveClient} from './google/drive_client';
import type {GoogleOAuthClient} from './google/oauth_client';
import type {ShareState, UploadState} from './views/share_form';

const DRIVE_FILE_SCOPE = 'https://www.googleapis.com/auth/drive.file';

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

  // Fix the name and destination now: the upload outlives this click.
  const fileName = share.fileName;
  const parentId = share.location === 'root' ? 'root' : share.location.id;
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

    share.fileId = result.value;
  } finally {
    share.upload = undefined;
  }
}

// Lets the user choose which Drive folder to upload to.
export async function pickUploadFolder(
  oauth: GoogleOAuthClient,
  drive: DriveClient,
  share: ShareState,
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

  share.location = picked.value;
}

// Opens Drive's sharing settings for the uploaded file.
export async function openGDriveSharingDialog(
  oauth: GoogleOAuthClient,
  drive: DriveClient,
  share: ShareState,
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
  const result = drive.openSharingDialog(auth.value, fileId);
  if (!result.ok) {
    share.error = {
      message: "Couldn't open the Google Drive sharing dialog.",
      ...result.error,
    };
  }
}
