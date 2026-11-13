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

import type {App} from '../../../public/app';
import {
  alertConnectFailed,
  alertDownloadFailed,
  alertPickerFailed,
} from '../dialogs';
import type {DriveClient} from '../google/drive_client';
import type {GoogleOAuthClient} from '../google/oauth_client';

const DRIVE_FILE_SCOPE = 'https://www.googleapis.com/auth/drive.file';

// Opens a file the user chooses from Google Drive, when they click the sidebar
// button. This is the whole flow, start to finish: each step either moves on
// to the next or tells the user why it could not.
export async function openFileFromSidebar(
  app: App,
  oauth: GoogleOAuthClient,
  drive: DriveClient,
) {
  // Authenticate with the oauth client and obtain an access token. This runs
  // from the sidebar button's click, and nothing is awaited before this call,
  // so the browser allows the Google popup.
  const auth = await oauth.authenticate([DRIVE_FILE_SCOPE]);
  if (!auth.ok) {
    await alertConnectFailed(auth.error);
    return;
  }
  const oauthToken = auth.value;

  // Let the user choose a file. Picking a file is also what authorizes the
  // app to read it.
  const picked = await drive.pickFile(oauthToken);
  if (!picked.ok) {
    if (picked.error.type === 'cancelled') return;
    await alertPickerFailed(picked.error);
    return;
  }
  const file = picked.value;

  // The filename is returned from the file picker too - use this to name the
  // trace.
  const filename = file.name ?? 'Google Drive file';

  // Download the file.
  const download = await drive.openFile(oauthToken, file.id);
  if (!download.ok) {
    await alertDownloadFailed(download.error, filename);
    return;
  }

  // Open the trace.
  app.openTraceFromBuffer({
    buffer: download.value,
    title: filename,
    fileName: filename,
    // ACL-protected Drive traces must not be re-uploaded to the public
    // perfetto-ui-data GCS bucket via permalinks or crash reports.
    shareable: false,
  });
}
