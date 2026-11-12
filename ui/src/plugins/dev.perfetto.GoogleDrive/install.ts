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
import {alertConnectFailed} from './dialogs';
import type {GoogleOAuthClient} from './gdrive/oauth_client';
import {alertModal} from './modal_prompt';

// Connects Perfetto to the user's Google account, and tells them how it went.
// Granting access is also what adds Perfetto to the "Open with" menu in the
// user's Google Drive. This is the whole flow, start to finish.
export async function installGDriveSupport(oauth: GoogleOAuthClient) {
  // Authenticate with the oauth client. This opens Google's popup unless there
  // is already a valid token, so it needs to run from a user gesture.
  const auth = await oauth.authenticate();
  if (!auth.ok) {
    await alertConnectFailed(auth.error);
    return;
  }

  await alertModal({
    key: 'GoogleDriveInstalled',
    title: 'Google Drive support installed',
    content: m('p', 'Perfetto is connected to your Google account.'),
  });
}
