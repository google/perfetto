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
import type {App} from '../../../public/app';
import {
  alertConnectFailed,
  alertDownloadFailed,
  alertPickerFailed,
} from '../dialogs';
import type {DriveClient} from '../google/drive_client';
import {parseGDriveStateParam} from '../google/drive_client';
import type {GoogleOAuthClient} from '../google/oauth_client';
import {promptModal} from '../../../widgets/modal';
import type {Setting} from '../../../public/settings';
import {Checkbox} from '../../../widgets/checkbox';

const DRIVE_FILE_SCOPE = 'https://www.googleapis.com/auth/drive.file';

// Opens the file named by Google Drive's "Open with" deep link, given the value
// of its `state` parameter. This is the whole flow, start to finish: each step
// either moves on to the next or tells the user why it could not.
export async function openFileFromDeeplink(
  app: App,
  oauth: GoogleOAuthClient,
  drive: DriveClient,
  state: string,
  hideAuthDialogSetting: Setting<boolean>,
) {
  // Parse the incoming state parameter from the "Open with" deep link.
  const parsed = parseGDriveStateParam(state);
  if (!parsed.ok) {
    // Failed to parse state parameter; ignore.
    return;
  }
  const {fileId, resourceKey} = parsed.value;

  // Authenticate with the oauth client and obtain an access token. Nothing was
  // clicked to get here, so the browser may block Google's popup. If it does,
  // ask the user to click a button and try again from that click.
  let auth = await oauth.authenticate([DRIVE_FILE_SCOPE]);
  if (!auth.ok && auth.error.type === 'popup_blocked') {
    const choice = await connectGoogleDrivePrompt();
    if (choice !== 'connect') return;

    // Try again. This runs shortly after the click, not inside it: see
    // promptModal() for why the popup is still allowed.
    auth = await oauth.authenticate([DRIVE_FILE_SCOPE]);
  }

  if (!auth.ok) {
    await alertConnectFailed(auth.error);
    return;
  }
  const oauthToken = auth.value;

  // We are committed to opening this file now, so consume the 'state' query
  // param. The router will append ?local_cache_key=xxx to the hash once the
  // trace finishes loading, so that a reload reopens the trace from the
  // browser's cache rather than from Drive.
  removeStateParamFromUrl();

  // Attempt to download the file and look up its name in parallel.
  const [fileMetaResult, initialDownloadResult] = await Promise.all([
    drive.getMeta(oauthToken, fileId, resourceKey),
    drive.openFile(oauthToken, fileId, resourceKey),
  ]);
  if (!fileMetaResult.ok && fileMetaResult.error.type !== 'access_denied') {
    console.warn(
      'Failed to fetch Google Drive file metadata',
      fileMetaResult.error,
    );
  }
  let fileName = fileMetaResult.ok ? fileMetaResult.value.name : undefined;
  let fileDownloadResult = initialDownloadResult;

  // If access is blocked, attempt to open it via the picker instead. The app
  // can only read files the user has opened or picked with it, so picking the
  // file is what authorizes it.
  if (
    !fileDownloadResult.ok &&
    fileDownloadResult.error.type === 'access_denied'
  ) {
    if (!hideAuthDialogSetting.get()) {
      const choice = await authAccessToFilePrompt(hideAuthDialogSetting);
      if (choice !== 'authorize') return;
    }

    const picked = await drive.requestFileAccess(oauthToken, fileId);
    if (!picked.ok) {
      if (picked.error.type === 'cancelled') return;
      await alertPickerFailed(picked.error);
      return;
    }

    // The picker reports the name of the file just authorized, which saves
    // looking it up.
    fileName = picked.value.name;

    // Try the download again.
    fileDownloadResult = await drive.openFile(oauthToken, fileId, resourceKey);
  }

  if (!fileDownloadResult.ok) {
    await alertDownloadFailed(fileDownloadResult.error, fileName);
    return;
  }

  fileName ??= 'Google Drive file';

  app.openTraceFromBuffer({
    buffer: fileDownloadResult.value,
    title: fileName,
    fileName: fileName,
    // ACL-protected Drive traces must not be re-uploaded to the public
    // perfetto-ui-data GCS bucket via permalinks or crash reports.
    shareable: false,
  });
}

// Removes the "Open with" deep link's 'state' query param from the address
// bar.
function removeStateParamFromUrl(): void {
  const url = new URL(window.location.href);
  url.searchParams.delete('state');
  history.replaceState(null, '', url.toString());
}

async function connectGoogleDrivePrompt() {
  return await promptModal({
    key: 'GoogleDriveConnect',
    title: 'Connect to Google Drive',
    content: [
      m(
        'p',
        'Perfetto needs to connect to your Google account to open this trace.',
      ),
      m(
        'p',
        'To skip this step in future, allow pop-ups for this site in your browser settings.',
      ),
    ],
    buttons: [
      {text: 'Connect', value: 'connect', primary: true},
      {text: 'Cancel', value: 'cancel'},
    ],
  });
}

async function authAccessToFilePrompt(hideAuthDialogSetting: Setting<boolean>) {
  return await promptModal({
    key: 'GoogleDriveAuthNeeded',
    title: 'Authorize access to file',
    content: () => [
      m(
        'p',
        'The first time you open a Google Drive file in Perfetto, you need to authorize access to it via the Google Drive picker. Click Authorize File Access to open the picker.',
      ),
      m(Checkbox, {
        label:
          "Don't show this again (go straight to the file picker in future)",
        checked: hideAuthDialogSetting.get(),
        onchange: () => hideAuthDialogSetting.set(!hideAuthDialogSetting.get()),
      }),
    ],
    buttons: [
      {text: 'Authorize File Access', value: 'authorize', primary: true},
      {text: 'Cancel', value: 'cancel'},
    ],
  });
}
