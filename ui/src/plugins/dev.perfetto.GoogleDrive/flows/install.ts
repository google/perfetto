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
import {alertConnectFailed} from '../dialogs';
import type {GoogleOAuthClient} from '../google/oauth_client';
import {alertModal} from '../../../widgets/modal';

// The OAuth scope that lets Perfetto be offered in Google Drive's "Open with"
// menu. It gives no access to any file.
const DRIVE_INSTALL_SCOPE = 'https://www.googleapis.com/auth/drive.install';

// Installs Perfetto as a Google Drive "app". This adds Perfetto to Drive's
// "Open with" menu for trace files (e.g. .trace, .pftrace, .perfetto-trace, and
// .gz variants). The supported extensions are configured in the perfetto-ui GCP
// project's Drive UI Integration settings, not here.
export async function installPerfettoAsDriveApp(oauth: GoogleOAuthClient) {
  // Ask for the install scope only; granting it is what adds Perfetto to the
  // menu. Note: This opens Google's popup unless there is already a valid
  // token, so it needs to run from a user gesture.
  const auth = await oauth.authenticate([DRIVE_INSTALL_SCOPE]);
  if (!auth.ok) {
    await alertConnectFailed(auth.error);
    return;
  }

  // If we get here - the installation succeeded.
  await alertModal({
    key: 'GoogleDriveInstalled',
    title: 'Google Drive support installed',
    content: m(
      'p',
      'Perfetto has been added to the "Open with" menu in your Google Drive.',
    ),
  });
}
