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
import {z} from 'zod';
import {assertDefined} from '../../base/assert';
import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import {DriveClient} from './google/drive_client';
import {GoogleOAuthClient} from './google/oauth_client';
import {loadGDriveConfig} from './gdrive_config';
import {installPerfettoAsDriveApp} from './flows/install';
import {alertModal} from '../../widgets/modal';
import {openFileFromSidebar} from './flows/open_from_sidebar';
import {openFileFromDeeplink} from './flows/open_from_deeplink';

export interface GDriveClients {
  readonly oauth: GoogleOAuthClient;
  readonly drive: DriveClient;
}

export default class GoogleDrivePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.GoogleDrive';
  static readonly description = 'Open traces from Google Drive';

  static onActivate(app: App) {
    // Google clients are odd - we must run them as external scripts so loading
    // their code naturally is asynchronous. We don't want to delay startup so
    // kick off the load async and disable features until they've loaded.
    let clients: GDriveClients | undefined;
    let initFinished = false;
    const clientsPromise = initGDrive(app).then((c) => {
      clients = c;
      initFinished = true;
      return c;
    });

    // Why the sidebar button cannot be used yet, or false if it can. The
    // sidebar shows the reason when a disabled button is clicked.
    function unavailableReason(): string | false {
      if (clients !== undefined) return false;
      return initFinished
        ? 'Google Drive support could not be loaded.'
        : 'Google Drive support is still loading.';
    }

    // Adds Perfetto to the "Open with" menu in the user's Google Drive.
    app.commands.registerCommand({
      id: `dev.perfetto.GoogleDriveInstall`,
      name: 'Install Google Drive support',
      callback: async () => {
        const clients = await clientsPromise;
        if (!clients) {
          alertGDriveNotAvailable();
          return;
        }
        await installPerfettoAsDriveApp(clients.oauth);
      },
    });

    const hideAuthDialogSetting = app.settings.register({
      id: `${GoogleDrivePlugin.id}#HideAuthDialog`,
      name: 'Google Drive: Hide file authorization dialog',
      description:
        'Hide the confirmation dialog and go straight to the Google Drive ' +
        'file picker when authorizing access to a trace from a deep link.',
      schema: z.boolean(),
      defaultValue: false,
    });

    let openFlowInProgress = false;

    app.sidebar.addMenuItem({
      section: 'trace_files',
      text: 'Open from GDrive',
      icon: 'drive_export',
      badge: 'preview',
      sortOrder: 1.25,
      // Disabled until everything it needs has loaded. This is a callback as
      // that only happens some time after startup.
      disabled: () => unavailableReason(),
      action: async () => {
        assertDefined(clients); // The button is disabled until this is set

        if (openFlowInProgress) {
          return;
        }

        openFlowInProgress = true;
        try {
          await openFileFromSidebar(app, clients.oauth, clients.drive);
        } finally {
          openFlowInProgress = false;
        }
      },
    });

    // When opening a trace directly from Google drive's UI, it passes the file
    // id and the resource keys in a hard-coded arg called 'state'. Intercept
    // this and load the file if these args are passed.
    const state = app.initialRouteArgs['state'];
    clientsPromise.then((clients) => {
      if (typeof state === 'string') {
        if (!clients) {
          alertGDriveNotAvailable();
          return;
        }

        openFileFromDeeplink(
          app,
          clients.oauth,
          clients.drive,
          state,
          hideAuthDialogSetting,
        );
      }
    });
  }
}

async function alertGDriveNotAvailable() {
  await alertModal({
    key: 'GoogleDriveUnavailable',
    title: 'Google Drive unavailable',
    content: m(
      'p',
      'Opening traces from Google Drive is not available right now.',
    ),
  });
}

// Loads everything Google Drive support needs, in order: the config, Google's
// sign-in library (for the OAuth client), then Google's API loader and its
// picker module (for the Drive client). This runs once, at startup, and
// nothing in it opens a popup.
//
// Returns undefined if any step fails. This is deliberately quiet (a console
// error only): it runs on every page load, including for users who are offline
// or block Google's scripts and never asked for Google Drive. The sidebar
// buttons stay disabled and the "Open with" deep link reports that Google
// Drive is unavailable.
async function initGDrive(app: App): Promise<GDriveClients | undefined> {
  try {
    const config = await loadGDriveConfig();
    const oauth = await GoogleOAuthClient.create({
      clientId: config.clientId,
    });
    const drive = await DriveClient.create({
      appId: config.appId,
      apiKey: config.apiKey,
    });
    return {oauth, drive};
  } catch (e) {
    console.error('Failed to initialize Google Drive', e);
    return undefined;
  } finally {
    app.raf.scheduleFullRedraw();
  }
}
